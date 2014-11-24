cmake_minimum_required(VERSION 2.6)

set(CXX_FLAGS
-std=c++1y
-g
-Wall
-Wextra
-Werror
-Wconversion
-Wshadow
)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    list(INSERT CXX_FLAGS 1 -stdlib=libc++)
endif()

string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")

find_package(wild REQUIRED)
include_directories(SYSTEM ${wild_INCLUDE_DIR})
link_directories(${wild_LIBRARY_DIR})

add_subdirectory(src stp)
add_subdirectory(examples)
