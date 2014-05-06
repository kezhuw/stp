include_directories(AFTER  ${PROJECT_SOURCE_DIR}/)

set(net_SRCS
tcp.cpp
)

add_library(stp_net STATIC ${net_SRCS})
target_link_libraries(stp_net pthread)
