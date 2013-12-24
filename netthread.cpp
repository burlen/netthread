#include <iostream>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <unistd.h>
using namespace std;

// to compile a.out
// g++ net.cpp -lpthread

// Description
// Milli-second resolution timer. This will be
// executed in a pthread that we wait on. If
// the FQDN reverse DNS lookup is not finished
// by the time our timer does we'll kill it and
// just use gethostname.
static
void *timer(void *msec)
{
  timespec rem;
  rem.tv_sec = 0;
  rem.tv_nsec = 1000000*(*(int*)msec);
  while ((nanosleep(&rem, &rem)==-1) && (errno==EINTR)) {}
  return NULL;
}

// Description:
// Data used by the rDNS lookup thread.
// The system's hostname is determined
// in the constructor if the rDNS lookup
// fails this will be used.
class HostInfo
{
public:
  HostInfo();

  // Description:
  // Set the timer thread id. This thread will
  // be killed once the FQDN is successfully
  // set.
  void SetTimerThreadId(pthread_t tid){ this->TimerTid = tid; }

  // Description:
  // Return the fqdn or hostname if the fqdn
  // was not found or timed out. Setting the
  // Fqdn clear the error flags.
  const string &GetFqdn();
  void SetFqdn(const string &fqdn);

  // Description:
  // Return the system's hostname.
  const string &GetHostname(){ return this->Hostname; }

  // Description:
  // Return the error flag. 0 indicates that
  // the FQDN was succesfully identified.
  int GetError(){ return this->Error; }
  void SetError(int err){ this->Error = err; }

private:
  string Hostname;
  string Fqdn;
  int Error;
  pthread_t TimerTid;
};

// --------------------------------------------------------------------------
HostInfo::HostInfo()
{
  this->Error = 0;
  this->TimerTid = 0;
  int ierr=0;
  char base[NI_MAXHOST];
  ierr=gethostname(base,NI_MAXHOST);
  if (ierr)
    {
    this->Error=-1;
    }
  this->Hostname=base;
  this->Fqdn=base;
}

// --------------------------------------------------------------------------
const string &HostInfo::GetFqdn()
{
  if (this->Error)
    {
    return this->Hostname;
    }
  return this->Fqdn;
}

// --------------------------------------------------------------------------
void HostInfo::SetFqdn(const string &fqdn)
{
  this->Fqdn = fqdn;
  this->Error = 0;
  pthread_cancel(this->TimerTid);
}

// **************************************************************************
void *getfqdn(void *tdata)
{
  // NOTE
  // this is the function from kwsys::SystemInformation used by PV

  // gethostname typical returns an alias for loopback interface
  // we want the fully qualified domain name. Because there are
  // any number of interfaces on this system we look for the
  // first of these that contains the name returned by gethostname
  // and is longer. failing that we return gethostname and indicate
  // with a failure code. Return of a failure code is not necessarilly
  // an indication of an error. for instance gethostname may return
  // the fully qualified domain name, or there may not be one if the
  // system lives on a private network such as in the case of a cluster
  // node.

  HostInfo *info=static_cast<HostInfo*>(tdata);
  std::string base = info->GetHostname();
  size_t baseSize = base.size();

  int ierr=0;
  struct ifaddrs *ifas;
  struct ifaddrs *ifa;
  ierr=getifaddrs(&ifas);
  if (ierr)
    {
    info->SetError(-2);
    return NULL;
    }

  for (ifa=ifas; ifa!=NULL; ifa=ifa->ifa_next)
    {
    int fam = ifa->ifa_addr? ifa->ifa_addr->sa_family : -1;
    if ((fam==AF_INET) || (fam==AF_INET6))
      {
      char host[NI_MAXHOST]={'\0'};

      const size_t addrlen
        = (fam==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6));

      ierr=getnameinfo(
            ifa->ifa_addr,
            static_cast<socklen_t>(addrlen),
            host,
            NI_MAXHOST,
            NULL,
            0,
            NI_NAMEREQD);
      if (ierr)
        {
        // don't report the failure now since we may succeed on another
        // interface. If all attempts fail then return the failure code.
        info->SetError(-3);
        continue;
        }

      std::string candidate=host;
      if ((candidate.find(base)!=std::string::npos) && baseSize<candidate.size())
        {
        // success, stop now.
        info->SetFqdn(candidate);
        break;
        }
      }
    }
  freeifaddrs(ifas);
  return NULL;
}

// **************************************************************************
int main(int argc, char **argv)
{
  if (argc != 2)
    {
    cerr << "error: a.out [timeout msec]" << endl;
    return -1;
    }

  int ierr = 0;
  // start the timer with the msecs passed on the command line
  pthread_t timerTid;
  int timeout = atoi(argv[1]); // msec
  ierr = pthread_create(&timerTid, NULL, timer, &timeout);
  if (ierr)
    {
    cerr << "error creating timer thread" << endl;
    return -2;
    }

  // start the potentially slow rdns lookup
  pthread_t rDnsTid;
  HostInfo info;
  info.SetTimerThreadId(timerTid);
  ierr = pthread_create(&rDnsTid, NULL, getfqdn, &info);
  if (ierr)
    {
    cerr << "error creating rdns thread" << endl;
    return -1;
    }


  // block on the timer
  ierr = pthread_join(timerTid, NULL);
  if (ierr)
    {
    cerr << "error waiting for timer" << endl;
    return -3;
    }

  // timer's finished see if the rdns thread finished
  bool rdnsSuccess = false;
  if (pthread_cancel(rDnsTid) == ESRCH)
    {
    // the rdns lookup thread has finished
    // we could successfully use the data it collected
    rdnsSuccess = true;
    }

  // cleanup rdns thread
  ierr = pthread_join(rDnsTid, NULL);
  if (ierr)
    {
    cerr << "error waiting for rdns thread" << endl;
    return -5;
    }

  // report what happened
  if (rdnsSuccess)
    {
    if (info.GetError())
      {
      cerr << "failed to find a match falling back to gethostname" << endl;
      }
    }
  else
    {
    cerr << "rdns took too long, falling back to gethostname" << endl;
    }

  cerr << "fqdn=" << info.GetFqdn() << endl;

  return 0;
}
