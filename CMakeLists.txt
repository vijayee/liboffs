cmake_minimum_required(VERSION 3.22)
project(offs C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer CACHE PATH "Path to poll-dancer library")
set(blake3_SOURCE_DIR deps/BLAKE3)
set(xxHash_SOURCE_DIR deps/xxHash)
set(hashmap_SOURCE_DIR deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/libcbor)

file(GLOB_RECURSE C_SRC "src/*/*.c")
list(FILTER C_SRC EXCLUDE REGEX "relay_server_main\\.c$")
list(FILTER C_SRC EXCLUDE REGEX "test_.*\\.c$")

file(GLOB_RECURSE C_INC "src/*/*.h")

include(CTest)
add_subdirectory(deps)


include(ExternalProject)
set(LIBCBOR_BUILD_PATH  ${CMAKE_BINARY_DIR}/deps/libcbor)
set(LIBCBOR_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor)
set(LIBCBOR_SRC_PATH    ${LIBCBOR_ROOT}/src)
set(LIBCBOR_LIB_PATH    ${LIBCBOR_ROOT}/lib)

set(LIBCBOR_CONFIGURE   cd ${LIBCBOR_ROOT} && cmake -B${LIBCBOR_BUILD_PATH} -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=${LIBCBOR_BUILD_PATH} -DCMAKE_INSTALL_PREFIX=${LIBCBOR_BUILD_PATH} -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCBOR_PRETTY_PRINTER=OFF -DWITH_EXAMPLES=OFF .)
set(LIBCBOR_MAKE        cd ${LIBCBOR_BUILD_PATH} && make)
set(LIBCBOR_INSTALL     cd ${LIBCBOR_BUILD_PATH} && make install)

ExternalProject_Add(libcbor
        SOURCE_DIR              ${LIBCBOR_ROOT}
        CONFIGURE_COMMAND       ${LIBCBOR_CONFIGURE}
        BUILD_COMMAND           ${LIBCBOR_MAKE}
        INSTALL_COMMAND         ${LIBCBOR_INSTALL}
        BUILD_BYPRODUCTS        ${LIBCBOR_BUILD_PATH}/libcbor.a
        BINARY_DIR              ${LIBCBOR_BUILD_PATH}

)
add_library(cbor STATIC IMPORTED)
set_target_properties(cbor PROPERTIES IMPORTED_LOCATION ${LIBCBOR_BUILD_PATH}/libcbor.a
  INTERFACE_INCLUDE_DIRECTORIES ${LIBCBOR_BUILD_PATH}/include)
add_dependencies(cbor libcbor)


# http-parser
add_library(http-parser STATIC deps/http-parser/http_parser.c)
target_include_directories(http-parser PUBLIC deps/http-parser)

# cJSON
add_library(cjson STATIC deps/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC deps/cJSON)

add_library(offs STATIC ${C_SRC})
add_dependencies(offs cbor)
add_dependencies(offs http-parser)
target_include_directories(offs PRIVATE ${blake3_SOURCE_DIR}/c)
target_include_directories(offs PRIVATE ${C_INC})
target_include_directories(offs PRIVATE ${xxHash_SOURCE_DIR})
target_include_directories(offs PRIVATE ${hashmap_SOURCE_DIR}/include)
target_include_directories(offs PRIVATE ${POLL_DANCER_ROOT}/include)
target_include_directories(offs PRIVATE ${POLL_DANCER_ROOT}/src)
target_include_directories(offs PUBLIC ${LIBCBOR_BUILD_PATH}/include)
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/cJSON)
target_link_libraries(offs PRIVATE ${POLL_DANCER_ROOT}/build/libpoll_dancer.a)
target_link_libraries(offs PRIVATE http-parser)
target_link_libraries(offs PRIVATE cjson)
target_link_libraries(offs PRIVATE crypt)

# Standalone C test for OFD resolver (not gtest-based)
add_executable(test_offs_ofd_resolver
  src/ClientLibs/c/test_offs_ofd_resolver.c
)
add_dependencies(test_offs_ofd_resolver cbor)
add_dependencies(test_offs_ofd_resolver offs)
target_link_libraries(test_offs_ofd_resolver PRIVATE offs)
target_link_libraries(test_offs_ofd_resolver PRIVATE ssl crypto)
target_link_libraries(test_offs_ofd_resolver PUBLIC cbor)
target_include_directories(test_offs_ofd_resolver PRIVATE ${C_INC})
target_include_directories(test_offs_ofd_resolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(test_offs_ofd_resolver PUBLIC ${LIBCBOR_BUILD_PATH}/include)

# Peer info unit test
add_executable(test_peer_info
  test/test_peer_info.c
)
add_dependencies(test_peer_info cbor)
add_dependencies(test_peer_info blake3)
add_dependencies(test_peer_info offs)
target_link_libraries(test_peer_info PRIVATE offs)
target_link_libraries(test_peer_info PRIVATE ssl crypto)
target_link_libraries(test_peer_info PRIVATE blake3)
target_link_libraries(test_peer_info PUBLIC cbor)
target_include_directories(test_peer_info PRIVATE ${C_INC})
target_include_directories(test_peer_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(test_peer_info PUBLIC ${LIBCBOR_BUILD_PATH}/include)

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

# libqrencode for QR code generation (optional)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(QRENCODE QUIET libqrencode)
endif()
if(QRENCODE_FOUND)
  target_compile_definitions(offs PRIVATE HAS_QRENCODE)
  target_include_directories(offs PRIVATE ${QRENCODE_INCLUDE_DIRS})
  target_link_libraries(offs PRIVATE ${QRENCODE_LIBRARIES})
  message(STATUS "libqrencode found — QR code generation enabled")
else()
  message(STATUS "libqrencode not found — QR code generation disabled")
endif()

add_subdirectory(src/Platform)
add_subdirectory(src/Network/Relay)
add_subdirectory(tools/offs-ca)
add_subdirectory(test)
add_subdirectory(examples)