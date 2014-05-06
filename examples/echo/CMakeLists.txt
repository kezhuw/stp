project(echo)

include_directories(AFTER ${stp_SOURCE_DIR})

add_executable(client client.cpp)
target_link_libraries(client stp)

add_executable(server server.cpp)
target_link_libraries(server stp)
