cmake_minimum_required(VERSION 3.29)
project(offs C CXX)

set(CMAKE_C_STANDARD 11)
set(C_SRC ""
        src/BlockCache/frand.c
        src/Util/allocator.c
        src/Util/log.h
        src/BlockCache/block.h
        src/BlockCache/block.h
        src/BlockCache/block.h)
set(C_INC "")
file(GLOB_RECURSE C_SRC "src/*/*.c" )
file(GLOB_RECURSE C_INC "/src/*/*.h")

file(GLOB_RECURSE BLAKE3_INC "/deps/BLAKE3/c/*.h")
file(GLOB_RECURSE BLAKE3_SRC "/deps/BLAKE3/c/*.c")

add_library(offs STATIC
        ${C_SRC})

target_include_directories(offs PUBLIC ${BLAKE3_INC})
target_link_libraries(offs ${BLAKE3_SRC})
target_include_directories(offs PUBLIC ${C_INC})
find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    # Link against the OpenSSL libraries
    target_link_libraries(offs ${OPENSSL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenSSL not found.")
endif()
include(CTest)
add_subdirectory(deps)
add_subdirectory(test)

include_directories()

