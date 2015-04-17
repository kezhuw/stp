cmake_minimum_required(VERSION 2.6)

set(CXX_FLAGS
-std=c++1y
-stdlib=libstdc++
-g
-Wall
-Wextra
-Werror
-Wconversion
-Wshadow
)

string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")

find_package(wild REQUIRED)
include_directories(SYSTEM ${wild_INCLUDE_DIR})
link_directories(${wild_LIBRARY_DIR})

add_subdirectory(src stp)
add_subdirectory(examples)
