cmake_minimum_required(VERSION 2.6)
project(stp)

set(stp_SRCS
process.cpp
context.cpp
message.cpp
sched.cpp
io.cpp
timer.cpp
fd_epoll.cpp
)

add_library(stp STATIC ${stp_SRCS})
target_link_libraries(stp pthread wild stp_net)

add_subdirectory(net)
