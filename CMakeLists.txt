cmake_minimum_required(VERSION 3.29)
project(offs C CXX)

set(CMAKE_C_STANDARD 11)
set(C_SRC "")
set(C_INC "")
file(GLOB_RECURSE C_SRC "src/*/*.c" )
file(GLOB_RECURSE C_INC "/src/*/*.h")


add_library(offs STATIC
        ${C_SRC})
target_include_directories(offs PUBLIC ${C_INC})
include(CTest)
add_subdirectory(deps)
add_subdirectory(test)
