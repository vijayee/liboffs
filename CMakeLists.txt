cmake_minimum_required(VERSION 3.22)
project(offs C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(blake3_SOURCE_DIR deps/BLAKE3)
set(xxHash_SOURCE_DIR deps/xxHash)
set(hashmap_SOURCE_DIR deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/libcbor)

file(GLOB_RECURSE C_SRC "src/*/*.c")
list(FILTER C_SRC EXCLUDE REGEX "relay_server_main\\.c$")

file(GLOB_RECURSE C_INC "src/*/*.h")

include(CTest)
add_subdirectory(deps)


include(ExternalProject)
set(LIBCBOR_BUILD_PATH  ${CMAKE_BINARY_DIR}/deps/libcbor)
set(LIBCBOR_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor)
set(LIBCBOR_SRC_PATH    ${LIBCBOR_ROOT}/src)
set(LIBCBOR_LIB_PATH    ${LIBCBOR_ROOT}/lib)

set(LIBCBOR_CONFIGURE   cd ${LIBCBOR_ROOT} && cmake -B${LIBCBOR_BUILD_PATH} -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=${LIBCBOR_BUILD_PATH} -DCMAKE_INSTALL_PREFIX=${LIBCBOR_BUILD_PATH} -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCBOR_PRETTY_PRINTER=OFF -DWITH_EXAMPLES=OFF .)
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
add_dependencies(cbor libcbor)


# http-parser
add_library(http-parser STATIC deps/http-parser/http_parser.c)
target_include_directories(http-parser PUBLIC deps/http-parser)

add_library(offs STATIC ${C_SRC})
add_dependencies(offs cbor)
add_dependencies(offs http-parser)
target_include_directories(offs PRIVATE ${blake3_SOURCE_DIR}/c)
target_include_directories(offs PRIVATE ${C_INC})
target_include_directories(offs PRIVATE ${xxHash_SOURCE_DIR})
target_include_directories(offs PRIVATE ${hashmap_SOURCE_DIR}/include)
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/include)
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/src)
target_include_directories(offs PUBLIC ${LIBCBOR_BUILD_PATH}/include)
target_link_libraries(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a)
target_link_libraries(offs PRIVATE http-parser)
target_link_libraries(offs PRIVATE crypt)

# msquic QUIC library (conditional — built if submodule is present)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/CMakeLists.txt)
  set(QUIC_DISABLE_WERROR ON CACHE BOOL "" FORCE)
  set(QUIC_TLS_OPENSSL_COMPAT_DEFINE "QUIC_TLS_OPENSSL_COMPAT" CACHE STRING "" FORCE)
  add_subdirectory(deps/msquic)
  target_compile_definitions(offs PRIVATE HAS_MSQUIC)
  target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc)
  target_link_libraries(offs PRIVATE msquic::msquic msquic::platform)
endif()

find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    target_link_libraries(offs PRIVATE ${OPENSSL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenSSL not found.")
endif()
add_subdirectory(src/Platform)
add_subdirectory(src/Network/Relay)
add_subdirectory(test)
add_subdirectory(examples)