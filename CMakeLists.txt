project(netthread)

add_executable(netthread netthread.cpp)
target_link_libraries(netthread pthread)
