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
        src/Workers/pool.c
        src/BlockCache/fibonacci.h
        src/BlockCache/fibonacci.c
        src/BlockCache/index.h
        src/BlockCache/index.c
        src/Util/hash.h
        src/Util/hash.c
        src/Util/vec.h
        src/Util/vec.c
        src/BlockCache/wal.h
        src/Util/mkdir_p.c
        src/Util/mkdir_p.h
        src/BlockCache/wal.c
        src/BlockCache/wal.c
        src/Util/path_join.h
        src/Util/path_join.c)
set(blake3_SOURCE_DIR deps/BLAKE3)
set(xxHash_SOURCE_DIR deps/xxHash)
set(hashmap_SOURCE_DIR deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/libcbor)
set(C_INC "")
file(GLOB_RECURSE C_SRC "src/*/*.c" )
file(GLOB_RECURSE C_INC "/src/*/*.h")

include(CTest)
add_subdirectory(deps)


include(ExternalProject)
set(LIBCBOR_BUILD_PATH  ${CMAKE_BINARY_DIR}/deps/libcbor)
set(LIBCBOR_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor)
set(LIBCBOR_SRC_PATH    ${LIBCBOR_ROOT}/src)
set(LIBCBOR_LIB_PATH    ${LIBCBOR_ROOT}/lib)

set(LIBCBOR_CONFIGURE   cd ${LIBCBOR_ROOT} && cmake -B${LIBCBOR_BUILD_PATH} -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=${LIBCBOR_BUILD_PATH} -DCMAKE_INSTALL_PREFIX=${LIBCBOR_BUILD_PATH} -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCBOR_PRETTY_PRINTER=OFF -DWITH_EXAMPLES=OFF .)
set(LIBCBOR_MAKE        cd ${LIBCBOR_ROOT} && make)
set(LIBCBOR_INSTALL     cd ${LIBCBOR_ROOT} && make install)

ExternalProject_Add(libcbor
        SOURCE_DIR              ${LIBCBOR_ROOT}
        CONFIGURE_COMMAND       ${LIBCBOR_CONFIGURE}
        BUILD_COMMAND           ${LIBCBOR_MAKE}
        INSTALL_COMMAND         ${LIBCBOR_INSTALL}
        BUILD_BYPRODUCTS        ${LIBCBOR_BUILD_PATH}/libcbor.a
        BINARY_DIR              ${LIBCBOR_BUILD_PATH}

)
add_library(cbor STATIC IMPORTED)
set_target_properties(cbor PROPERTIES IMPORTED_LOCATION ${LIBCBOR_BUILD_PATH}/libcbor.a)


add_library(offs STATIC ${C_SRC})
add_dependencies(offs cbor)
target_include_directories(offs PRIVATE ${blake3_SOURCE_DIR}/c)
target_include_directories(offs PRIVATE ${C_INC})
target_include_directories(offs PRIVATE ${xxHash_SOURCE_DIR})
target_include_directories(offs PRIVATE ${hashmap_SOURCE_DIR}/include)
target_include_directories(offs PUBLIC ${LIBCBOR_BUILD_PATH}/include)
#target_link_libraries(offs PUBLIC ${hashmap_SOURCE_DIR}/libhashmap)

#target_include_directories(offs PUBLIC ${BLAKE3_INC})
#target_link_libraries(offs ${BLAKE3_SRC})


find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    # Link against the OpenSSL libraries
    target_link_libraries(offs PRIVATE ${OPENSSL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenSSL not found.")
endif()
add_subdirectory(test)



