cmake_minimum_required(VERSION 3.29)
project(offs C CXX)

set(CMAKE_C_STANDARD 11)
set(C_SRC ""
        src/BlockCache/frand.c
        src/Util/allocator.c
        src/Util/log.h
        src/BlockCache/block.h
        src/BlockCache/block.h
        src/BlockCache/block.h
        src/Workers/promise.h
        src/Workers/priority.h
        src/Workers/priority.c
        src/Workers/work.c
        src/Workers/queue.h
        src/Workers/queue.h
        src/Workers/queue.c
        src/Workers/pool.h
        src/Workers/pool.c)
set(blake3_SOURCE_DIR deps/BLAKE3)
set(C_INC "")
file(GLOB_RECURSE C_SRC "src/*/*.c" )
file(GLOB_RECURSE C_INC "/src/*/*.h")

include(CTest)
add_subdirectory(deps)
add_subdirectory(test)

#file(GLOB_RECURSE BLAKE3_INC "/deps/BLAKE3/c/*.h")
#file(GLOB_RECURSE BLAKE3_SRC "/deps/BLAKE3/c/*.c")
#message(STATUS "${BLAKE3_INC}")

add_library(offs STATIC ${C_SRC})
#target_link_libraries(offs ${blake3_SOURCE_DIR}/libbla)
target_include_directories(offs PUBLIC ${blake3_SOURCE_DIR}/c)
target_include_directories(offs PUBLIC ${C_INC})

#target_include_directories(offs PUBLIC ${BLAKE3_INC})
#target_link_libraries(offs ${BLAKE3_SRC})


find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    # Link against the OpenSSL libraries
    target_link_libraries(offs ${OPENSSL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenSSL not found.")
endif()




