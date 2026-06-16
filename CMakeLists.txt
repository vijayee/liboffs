cmake_minimum_required(VERSION 3.22)
project(offs C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# poll-dancer — built as an external project, same pattern as libcbor above.
# Pinned via the deps/poll-dancer git submodule (commit recorded in .gitmodules).
# Use ${CMAKE_COMMAND} --build instead of `make` so the build is portable across
# generators (Ninja, Visual Studio, Unix Makefiles).
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer CACHE PATH "Path to poll-dancer source")
set(POLL_DANCER_BUILD_PATH ${CMAKE_BINARY_DIR}/deps/poll-dancer)
set(POLL_DANCER_CONFIGURE ${CMAKE_COMMAND} -S ${POLL_DANCER_ROOT} -B ${POLL_DANCER_BUILD_PATH} -DCMAKE_BUILD_TYPE=Release -DPOLL_DANCER_BUILD_TESTS=OFF -DPOLL_DANCER_BUILD_EXAMPLES=OFF -DPOLL_DANCER_THREAD_SAFE=OFF)
set(POLL_DANCER_MAKE ${CMAKE_COMMAND} --build ${POLL_DANCER_BUILD_PATH} --target poll_dancer --config Release)
# poll-dancer's default install target writes to /usr/local — we only need the .a in BINARY_DIR, so skip install.
set(POLL_DANCER_INSTALL ${CMAKE_COMMAND} -E echo "Skipping poll-dancer install; using ${POLL_DANCER_BUILD_PATH}/libpoll_dancer.a")
include(ExternalProject)
# BUILD_BYPRODUCTS must match the actual generated library. On POSIX
# ninja generates libpoll_dancer.a in BINARY_DIR; on Windows the inner
# poll-dancer project uses the Visual Studio generator and drops
# poll_dancer.lib in a per-config subdir (Release/ on Release builds).
if(MSVC)
  set(POLL_DANCER_BYPRODUCT "${POLL_DANCER_BUILD_PATH}/Release/poll_dancer.lib")
else()
  set(POLL_DANCER_BYPRODUCT "${POLL_DANCER_BUILD_PATH}/libpoll_dancer.a")
endif()
ExternalProject_Add(poll-dancer-ext
  SOURCE_DIR ${POLL_DANCER_ROOT}
  CONFIGURE_COMMAND ${POLL_DANCER_CONFIGURE}
  BUILD_COMMAND ${POLL_DANCER_MAKE}
  INSTALL_COMMAND ${POLL_DANCER_INSTALL}
  BUILD_BYPRODUCTS ${POLL_DANCER_BYPRODUCT}
  BINARY_DIR ${POLL_DANCER_BUILD_PATH}
)
add_library(poll-dancer STATIC IMPORTED)
if(MSVC)
  # On Windows the poll-dancer subproject is built with the Visual Studio
  # generator, which places the import library in a per-configuration
  # subdirectory using MSVC naming conventions (poll_dancer.lib, no
  # "lib" prefix). Map both Debug and Release configurations to the
  # corresponding output directory.
  set_target_properties(poll-dancer PROPERTIES
    IMPORTED_LOCATION_DEBUG   "${POLL_DANCER_BUILD_PATH}/Debug/poll_dancer.lib"
    IMPORTED_LOCATION_RELEASE "${POLL_DANCER_BUILD_PATH}/Release/poll_dancer.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${POLL_DANCER_ROOT}/include;${POLL_DANCER_ROOT}/src")
else()
  set_target_properties(poll-dancer PROPERTIES
    IMPORTED_LOCATION ${POLL_DANCER_BUILD_PATH}/libpoll_dancer.a
    INTERFACE_INCLUDE_DIRECTORIES "${POLL_DANCER_ROOT}/include;${POLL_DANCER_ROOT}/src")
endif()
add_dependencies(poll-dancer poll-dancer-ext)
set(blake3_SOURCE_DIR deps/BLAKE3)
set(xxHash_SOURCE_DIR deps/xxHash)
set(hashmap_SOURCE_DIR deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/libcbor)

# MSVC does not enable C11 <stdatomic.h> by default — the atomic_compat.h header
# in this project includes it unconditionally, so the /experimental:c11atomics
# flag is required on MSVC builds. Other compilers ship <stdatomic.h> by default.
# Apply it globally to every C target in this file (offs, the standalone test
# executables below, and any targets defined in subdirectories added later).
# The flag must be set before add_library(offs) at the top of the file.
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

file(GLOB_RECURSE C_SRC "src/*/*.c")
list(FILTER C_SRC EXCLUDE REGEX "relay_server_main\\.c$")
list(FILTER C_SRC EXCLUDE REGEX "test_.*\\.c$")
# The Update module uses fork()/execlp() to spawn a standalone updater binary
# which is not portable to Windows. Allow disabling it on WIN32 builds.
if(WIN32)
  list(FILTER C_SRC EXCLUDE REGEX "src/Update/")
  # The Unix transport now compiles on Windows: it dispatches to AF_UNIX
  # (Windows 10 1803+) or named pipes (older Windows) via platform_local.
  # The TCP/WS transports still use POSIX-only socket APIs.
  list(FILTER C_SRC EXCLUDE REGEX "src/ClientAPI/TCP/")
  list(FILTER C_SRC EXCLUDE REGEX "src/ClientAPI/WS/")
  # The standalone C client library directly includes the WS frame header
  # and POSIX socket APIs. Excluded on Windows until a Winsock2 rewrite lands.
  list(FILTER C_SRC EXCLUDE REGEX "src/ClientLibs/c/")
  # The HTTP module is compiled on Windows via a minimal POSIX regex shim
  # (see src/Platform/platform_regex_compat.{h,c}). Routes match as literal
  # strings — no regex metacharacters. Leave the .c files in the build.
endif()

file(GLOB_RECURSE C_INC "src/*/*.h")
if(WIN32)
  # Drop headers from excluded source directories so files that include them
  # through transitive paths don't see POSIX-only references either.
  list(FILTER C_INC EXCLUDE REGEX "src/ClientAPI/TCP/")
  list(FILTER C_INC EXCLUDE REGEX "src/ClientAPI/WS/")
endif()

include(CTest)
add_subdirectory(deps)


include(ExternalProject)
set(LIBCBOR_BUILD_PATH  ${CMAKE_BINARY_DIR}/deps/libcbor)
set(LIBCBOR_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor)
set(LIBCBOR_SRC_PATH    ${LIBCBOR_ROOT}/src)
set(LIBCBOR_LIB_PATH    ${LIBCBOR_ROOT}/lib)

# Use ${CMAKE_COMMAND} --build instead of `make` so the build is portable across
# generators (Ninja, Visual Studio, Unix Makefiles).
set(LIBCBOR_CONFIGURE   ${CMAKE_COMMAND} -S ${LIBCBOR_ROOT} -B ${LIBCBOR_BUILD_PATH} -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=${LIBCBOR_BUILD_PATH} -DCMAKE_INSTALL_PREFIX=${LIBCBOR_BUILD_PATH} -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCBOR_PRETTY_PRINTER=OFF -DWITH_EXAMPLES=OFF)
set(LIBCBOR_MAKE        ${CMAKE_COMMAND} --build ${LIBCBOR_BUILD_PATH})
set(LIBCBOR_INSTALL     ${CMAKE_COMMAND} --build ${LIBCBOR_BUILD_PATH} --target install)

ExternalProject_Add(libcbor
        SOURCE_DIR              ${LIBCBOR_ROOT}
        CONFIGURE_COMMAND       ${LIBCBOR_CONFIGURE}
        BUILD_COMMAND           ${LIBCBOR_MAKE}
        INSTALL_COMMAND         ${LIBCBOR_INSTALL}
        BUILD_BYPRODUCTS        ${LIBCBOR_BUILD_PATH}/libcbor.a
        BINARY_DIR              ${LIBCBOR_BUILD_PATH}

)
add_library(cbor STATIC IMPORTED)
if(MSVC)
  # Same per-configuration subdirectory layout as poll-dancer above: the
  # libcbor Visual Studio generator project places its import library
  # at the top level using the MSVC naming convention.
  set_target_properties(cbor PROPERTIES
    IMPORTED_LOCATION ${LIBCBOR_BUILD_PATH}/cbor.lib
    INTERFACE_INCLUDE_DIRECTORIES "${LIBCBOR_ROOT}/src"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LIBCBOR_BUILD_PATH}/include")
else()
  set_target_properties(cbor PROPERTIES IMPORTED_LOCATION ${LIBCBOR_BUILD_PATH}/libcbor.a
    INTERFACE_INCLUDE_DIRECTORIES "${LIBCBOR_ROOT}/src"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LIBCBOR_BUILD_PATH}/include")
endif()
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
target_include_directories(offs PUBLIC ${LIBCBOR_BUILD_PATH}/include)
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/cJSON)
target_link_libraries(offs PUBLIC poll-dancer)
target_link_libraries(offs PRIVATE http-parser)
target_link_libraries(offs PRIVATE cjson)
# libcrypt is the POSIX crypt(3) helper. It is provided by glibc on Linux
# and by libxcrypt on some BSDs; MSVC has no equivalent (cryptographic
# primitives use bcrypt/CNG). Exclude it on Windows so the link line stays
# valid.
if(NOT WIN32)
  target_link_libraries(offs PRIVATE crypt)
endif()
# MSVC does not enable C11 <stdatomic.h> by default — the atomic_compat.h header
# in this project includes it unconditionally, so the /experimental:c11atomics
# flag is required on MSVC builds. Other compilers ship <stdatomic.h> by default.
# Apply it globally via add_compile_options near the top of the file (before
# add_library(offs)) so it propagates to every C target in this file and in
# any subdirectory added later (Platform, Network/Relay, tools, test, etc.).

# Standalone C test for OFD resolver (not gtest-based)
# The OFD resolver is excluded from offs.lib on Windows because it links
# offs_http_get (POSIX sockets). The test only exercises the pure helpers
# (offs_ofd_total_blocks, offs_ofd_build_recyclers), so compile the resolver
# source directly into the test target. We provide a stub offs_http_get that
# returns NULL since the test never triggers the HTTP path.
add_executable(test_offs_ofd_resolver
  src/ClientLibs/c/test_offs_ofd_resolver.c
)
add_dependencies(test_offs_ofd_resolver cbor)
add_dependencies(test_offs_ofd_resolver offs)
target_link_libraries(test_offs_ofd_resolver PRIVATE offs)
if(WIN32)
  target_sources(test_offs_ofd_resolver PRIVATE src/ClientLibs/c/offs_ofd_resolver.c)
  # Stubs for the POSIX-only HTTP path referenced by offs_ofd_resolver.c but
  # unused by the test (the test calls only offs_ofd_total_blocks and
  # offs_ofd_build_recyclers). The linker needs symbols even if they are
  # never executed.
  target_sources(test_offs_ofd_resolver PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/test_offs_ofd_stubs.c")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test_offs_ofd_stubs.c"
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c/offs_client.h\"
buffer_t* offs_http_get(const char* url) { (void)url; return NULL; }
")
endif()
target_link_libraries(test_offs_ofd_resolver PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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
target_link_libraries(test_peer_info PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(test_peer_info PRIVATE blake3)
target_link_libraries(test_peer_info PUBLIC cbor)
target_include_directories(test_peer_info PRIVATE ${C_INC})
target_include_directories(test_peer_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(test_peer_info PUBLIC ${LIBCBOR_BUILD_PATH}/include)

# msquic QUIC library (conditional — built if submodule is present)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/CMakeLists.txt)
  set(QUIC_DISABLE_WERROR ON CACHE BOOL "" FORCE)
  set(QUIC_TLS_OPENSSL_COMPAT_DEFINE "QUIC_TLS_OPENSSL_COMPAT" CACHE STRING "" FORCE)
  # msquic's CMakeLists unconditionally sets /WX (treat warnings as errors) on
  # Windows and does not honor QUIC_DISABLE_WERROR. Pre-set QUIC_WARNING_FLAGS
  # with /WX- before add_subdirectory so the warnings interface target picks
  # it up.
  if(MSVC)
    set(QUIC_WARNING_FLAGS /WX- /W4 /sdl /wd4206 CACHE INTERNAL "" FORCE)
  endif()
  add_subdirectory(deps/msquic)
  # Re-assert /WX- after add_subdirectory in case msquic re-set the variable
  # without FORCE inside its own CMakeLists. Re-assigning INTERFACE compile
  # options on the `warnings` target is the only path that actually affects
  # the compile line on the generated Visual Studio projects.
  if(MSVC AND TARGET warnings)
    set_property(TARGET warnings PROPERTY INTERFACE_COMPILE_OPTIONS "/WX-;/W4;/sdl;/wd4206")
  endif()
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