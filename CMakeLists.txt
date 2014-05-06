cmake_minimum_required(VERSION 2.6)

set(CXX_FLAGS
-std=c++1y
-stdlib=libc++
-g
-Wall
-Wextra
-Werror
-Wconversion
-Wshadow
)

string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")

add_subdirectory(src stp)
add_subdirectory(examples)
