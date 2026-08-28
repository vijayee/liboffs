cmake_minimum_required(VERSION 3.22)
project(offs LANGUAGES C CXX VERSION 0.1.0)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# msquic QUIC is a required submodule dependency (pinned in .gitmodules as
# deps/msquic). The project depends on it for QUIC / relay / WebTransport /
# NAT-detection and peer-to-peer networking, so the build fails entirely if
# the submodule is missing rather than degrading to no-op stubs. HAS_MSQUIC is
# always defined and msquic is always linked into the offs target.
#
# OFFS_ENABLE_QUIC is no longer an option: an explicit -DOFFS_ENABLE_QUIC=OFF
# invocation is rejected below as a hard error (it used to produce a silent
# network-less / no-P2P build, which is not a configuration this project
# supports).

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
  # The TCP/WS transports compile on Windows via the platform_socket shim
  # and an IOCP-safe accept/read path: poll platform_socket_accept each
  # loop iteration (IOCP can't deliver READ on a listening socket) and
  # defer watcher stop to the destroy stack so a pending IOCP completion
  # never touches freed memory.
  # The standalone C client library (src/ClientLibs/c/offs_client.c) now
  # builds on Windows via the platform_socket shim plus Winsock2 includes
  # for offs_http_get, so the whole src/ClientLibs/c/ directory is part of
  # offs.lib on every platform.
  # The HTTP module is compiled on Windows via a minimal POSIX regex shim
  # (see src/Platform/platform_regex_compat.{h,c}). Routes match as literal
  # strings — no regex metacharacters. Leave the .c files in the build.
endif()

file(GLOB_RECURSE C_INC "src/*/*.h")

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

# Vendored Solar Designer crypt_blowfish (deps/bcrypt submodule) — the bcrypt
# verify primitive, used by src/Util/bcrypt.c on every platform. The former
# glibc crypt_r path is gone, so bcrypt is a hard dependency on all platforms
# (including Linux); the if(EXISTS) guard gives a clear error when the submodule
# is not initialized instead of a cryptic link failure. Only crypt_blowfish.c is
# compiled — rg3's bcrypt.c (whose bcrypt_gensalt pulls in /dev/urandom +
# <unistd.h>/<fcntl.h>) is deliberately not built, since hash generation stays
# an external operator-side operation.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/bcrypt/crypt_blowfish/crypt_blowfish.c)
  add_library(bcrypt_os STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/bcrypt/crypt_blowfish/crypt_blowfish.c)
  target_include_directories(bcrypt_os PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/bcrypt/crypt_blowfish)
  if(MSVC)
    # Silence harmless size-conversion warnings (C4244/C4267) in the vendored
    # OpenBSD/Solar-Designer code. Scoped to this target so liboffs warnings
    # stay at their normal level.
    target_compile_options(bcrypt_os PRIVATE /W0)
  endif()
else()
  message(FATAL_ERROR "deps/bcrypt submodule missing. Run: git submodule update --init --recursive")
endif()

add_library(offs STATIC ${C_SRC})
# Propagate the project version as OFFS_VERSION so version.h's default
# "0.0.0" is only a fallback for non-CMake builds. PUBLIC so consumers link
# the version metadata. Update update_actor.c:183 reads this at runtime.
target_compile_definitions(offs PUBLIC OFFS_VERSION="${PROJECT_VERSION}")
# Opt-in AddressSanitizer instrumentation of the offs library only (not the
# external deps: msquic/poll-dancer/libcbor are add_subdirectory/ExternalProject
# targets that inherit CMAKE_C_FLAGS, so setting /fsanitize=address globally
# would force ASan onto them — slow and brittle. Scoping to the offs target
# instruments the code under test while leaving externals uninstrumented, which
# is the supported ASan usage: the test exe pulls the ASan runtime via its own
# link options and offs's instrumented objects resolve against it. /Z7 embeds
# debug info so ASan stack traces symbolicate. Default OFF; normal builds are
# unaffected.
option(OFFS_ENABLE_ASAN "Instrument the offs library with AddressSanitizer (MSVC) for debugging" OFF)
if(OFFS_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(offs PRIVATE /fsanitize=address /Z7)
  else()
    # GCC/Clang: -fsanitize=address with -g for symbolicated stack traces.
    # Scoping to the offs target (PRIVATE) keeps external deps uninstrumented,
    # matching the MSVC policy above. The test executable must also enable ASan
    # so the runtime is pulled in at link time — see OFFS_ENABLE_ASAN handling
    # in test/CMakeLists.txt.
    target_compile_options(offs PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(offs INTERFACE -fsanitize=address)
  endif()
endif()

# Optional UndefinedBehaviorSanitizer and ThreadSanitizer (GCC/Clang only).
# These are OFF by default and have no MSVC equivalent in this build. TSan and
# ASan are mutually exclusive — do not enable both in the same build.
option(OFFS_ENABLE_UBSAN "Instrument the offs library with UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(OFFS_ENABLE_UBSAN AND NOT MSVC)
  target_compile_options(offs PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
  target_link_options(offs INTERFACE -fsanitize=undefined)
endif()

option(OFFS_ENABLE_TSAN "Instrument the offs library with ThreadSanitizer (GCC/Clang). Mutually exclusive with OFFS_ENABLE_ASAN." OFF)
if(OFFS_ENABLE_TSAN AND NOT MSVC)
  if(OFFS_ENABLE_ASAN)
    message(FATAL_ERROR "OFFS_ENABLE_TSAN and OFFS_ENABLE_ASAN are mutually exclusive; enable only one.")
  endif()
  target_compile_options(offs PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(offs INTERFACE -fsanitize=thread)
endif()

# Release manifest signature verification: the ed25519 public key (PEM) used by
# update_verify_manifest to verify signed release manifests. Pass a path to a
# PEM file at configure time; the contents are embedded as a C string literal
# (newlines escaped) on the offs target. When unset, the verifier is fail-
# closed (update_verify_load_pubkey returns NULL and every verification fails)
# and a WARNING is emitted so the operator notices.
set(OFFS_RELEASE_PUBKEY "" CACHE STRING "Path to a PEM ed25519 public key for verifying release manifests (Prometheus-SCN release key)")
if(OFFS_RELEASE_PUBKEY)
  file(READ ${OFFS_RELEASE_PUBKEY} _offs_release_pubkey_pem)
  string(REPLACE "\n" "\\n" _offs_release_pubkey_escaped "${_offs_release_pubkey_pem}")
  target_compile_definitions(offs PRIVATE OFFS_RELEASE_PUBKEY="${_offs_release_pubkey_escaped}")
else()
  message(WARNING "OFFS_RELEASE_PUBKEY not set — self-update verification will be fail-closed (no release key compiled in)")
endif()

# Opt-in debug info (PDB + /Z7) for the offs library WITHOUT AddressSanitizer.
# Used by the test crash-catcher (test_crash_catch.h) to symbolicate a native
# faulting stack at native timing — ASan changes scheduler/allocation timing
# and can suppress reproduction of narrow races, so a native-with-symbols build
# is the tool that actually surfaces the crash site. /Z7 embeds debug info in
# the objects so frames inside the offs library symbolicate. Default OFF; normal
# Release builds are unaffected.
option(OFFS_TEST_DEBUG "Embed debug info (PDB + /Z7) in offs for native crash-diagnosis builds" OFF)
if(OFFS_TEST_DEBUG AND NOT OFFS_ENABLE_ASAN)
  target_compile_options(offs PRIVATE /Z7)
endif()
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
# bcrypt_os (vendored Solar Designer crypt_blowfish) backs bcrypt_check on
# every platform. The former glibc crypt_r/crypt dependency is removed —
# crypt_blowfish is the same implementation family glibc's crypt() delegates to
# for $2b$, so this unifies verify behavior across Linux/Windows/macOS with no
# behavior change on Linux.
target_link_libraries(offs PRIVATE bcrypt_os)
# MSVC does not enable C11 <stdatomic.h> by default — the atomic_compat.h header
# in this project includes it unconditionally, so the /experimental:c11atomics
# flag is required on MSVC builds. Other compilers ship <stdatomic.h> by default.
# Apply it globally via add_compile_options near the top of the file (before
# add_library(offs)) so it propagates to every C target in this file and in
# any subdirectory added later (Platform, Network/Relay, tools, test, etc.).

# Standalone C test for OFD resolver (not gtest-based). The resolver and
# offs_http_get both live in offs.lib on every platform (offs_client.c now
# builds on Windows via the platform_socket/Winsock2 shim), so the test links
# them from offs.lib — no per-target source or stub needed.
add_executable(test_offs_ofd_resolver
  src/ClientLibs/c/test_offs_ofd_resolver.c
)
add_dependencies(test_offs_ofd_resolver cbor)
add_dependencies(test_offs_ofd_resolver offs)
target_link_libraries(test_offs_ofd_resolver PRIVATE offs)
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

# msquic QUIC library — required submodule dependency. Built unconditionally
# when deps/msquic is present; a missing submodule is a loud, actionable
# FATAL_ERROR instead of a silent degrade to stub QUIC/relay/WT code. An
# explicit -DOFFS_ENABLE_QUIC=OFF (the former opt-out) is rejected as a hard
# error: msquic is no longer optional.
if(DEFINED OFFS_ENABLE_QUIC AND NOT OFFS_ENABLE_QUIC)
  message(FATAL_ERROR
    "OFFS_ENABLE_QUIC=OFF is no longer supported. msquic is a required "
    "submodule dependency (deps/msquic); the project depends on it for "
    "QUIC/relay/WebTransport/peer-to-peer networking. Remove "
    "-DOFFS_ENABLE_QUIC=OFF and run `git submodule update --init --recursive` "
    "to fetch deps/msquic.")
endif()

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
  target_compile_definitions(offs PUBLIC HAS_MSQUIC)
  target_include_directories(offs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc)
  target_link_libraries(offs PRIVATE msquic::msquic msquic::platform)
else()
  message(FATAL_ERROR
    "Required submodule deps/msquic is missing. msquic is a hard build "
    "dependency (QUIC/relay/WebTransport/peer-to-peer networking). Run: "
    "git submodule update --init --recursive")
endif()

# pcre2 — POSIX regex compatibility wrapper, used on Windows to back the
# http_route pattern compiler. Linux uses the system <regex.h> and does not
# need this. Pinned via deps/pcre2 git submodule (commit recorded in
# .gitmodules). The 8-bit code unit is enough for POSIX byte/UTF-8 patterns;
# the 16/32-bit variants, JIT, and CLI tools are disabled to keep the build
# minimal. The pcre2-posix static library provides regcomp/regexec/regfree
# symbols that match the POSIX <regex.h> API, so http_route.c is unchanged.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/pcre2/CMakeLists.txt)
  set(PCRE2_BUILD_PCRE2_8   ON  CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2_16  OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2_32  OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
  set(PCRE2_SUPPORT_JIT     OFF CACHE BOOL "" FORCE)
  add_subdirectory(deps/pcre2)
  if(WIN32)
    # The pcre2-8-static and pcre2-posix-static targets' INTERFACE_INCLUDE_DIRECTORIES
    # include deps/pcre2/src, but only when they are imported/linked via the
    # add_subdirectory above. The pcre2posix.h header is consumed by
    # src/Platform/platform_regex_compat.h, which is a public header (included
    # from src/ClientAPI/HTTP/http_route.h) — so any target that includes
    # http_route.h transitively needs the include path. Add it as PUBLIC.
    target_link_libraries(offs PRIVATE pcre2-8-static pcre2-posix-static)
    target_include_directories(offs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/pcre2/src)
  endif()
  if(WIN32)
    # wt_transport's Windows/Schannel server-credential path imports the PEM
    # cert + key into a transient cert store (PFXImportCertStore /
    # CertEnumCertificatesInStore / CertFreeCertificateContext / CertCloseStore,
    # all in crypt32.dll) and, on shutdown, deletes the auto-named CNG key
    # container PFXImportCertStore created (NCryptOpenStorageProvider /
    # NCryptOpenKey / NCryptDeleteKey / NCryptFreeObject, in ncrypt.dll). offs
    # is a static archive, so CMake propagates these PRIVATE dependencies to
    # every executable that links offs (testliboffs, the integration tests,
    # offsd).
    target_link_libraries(offs PRIVATE crypt32 ncrypt)
  endif()
endif()

find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    target_link_libraries(offs PRIVATE ${OPENSSL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenSSL not found.")
endif()

# libqrencode — QR encoder, vendored submodule (deps/libqrencode). Required:
# QR peer-info generation is a first-class client-API feature, not an optional
# extra, so a missing submodule is a loud error like deps/bcrypt. Built
# without PNG support and without CLI tools — only the core encoder is used,
# via src/QR/qr.c (added by the next task).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/libqrencode/CMakeLists.txt)
  set(WITH_TOOLS   OFF CACHE BOOL "" FORCE)
  set(WITH_TESTS   OFF CACHE BOOL "" FORCE)
  set(WITHOUT_PNG  ON  CACHE BOOL "" FORCE)
  # libqrencode turns its own tests back on whenever BUILD_TESTING is set
  # (its CMakeLists does `if(BUILD_TESTING) set(WITH_TESTS ON)`), which would
  # defeat the WITH_TESTS=OFF above and register ctest entries for binaries
  # that are never built. Shadow BUILD_TESTING with a plain directory-scoped
  # variable just for this add_subdirectory — the cache value the project's
  # own test suite (test/CMakeLists.txt) depends on stays untouched.
  set(_OFFS_QRENCODE_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  add_subdirectory(deps/libqrencode EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${_OFFS_QRENCODE_BUILD_TESTING})
  unset(_OFFS_QRENCODE_BUILD_TESTING)
  target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/libqrencode)
  target_link_libraries(offs PRIVATE qrencode)
else()
  message(FATAL_ERROR "deps/libqrencode submodule missing. Run: git submodule update --init --recursive")
endif()

# quirc — QR decoder, vendored submodule (deps/quirc). Upstream ships no
# CMakeLists.txt (Makefile-only, see deps/quirc/Makefile LIB_OBJ), so compile
# its four decoder sources (quirc.c, decode.c, identify.c, version_db.c)
# directly into a static library. Only decode is used (via src/QR/qr.c).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/quirc/lib/quirc.c)
  add_library(quirc STATIC
    deps/quirc/lib/quirc.c
    deps/quirc/lib/decode.c
    deps/quirc/lib/identify.c
    deps/quirc/lib/version_db.c)
  target_include_directories(quirc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/quirc/lib)
  # The default QUIRC_MAX_REGIONS=254 is too small for dense version-40 QR
  # codes (one region per isolated module): the region budget is exhausted
  # before the bottom-left finder pattern is labeled, making large payloads
  # undecodable. 65000 is near quirc's documented ceiling (65534); pixel type
  # widens to 32-bit automatically when the limit exceeds 254.
  target_compile_definitions(quirc PRIVATE QUIRC_MAX_REGIONS=65000)
  if(NOT MSVC)
    # identify.c uses rint/sqrt; MSVC's CRT provides both intrinsically.
    target_link_libraries(quirc PUBLIC m)
  endif()
  if(MSVC)
    # Silence size-conversion warnings in vendored code, matching the bcrypt target's policy.
    target_compile_options(quirc PRIVATE /W0)
  endif()
  target_link_libraries(offs PRIVATE quirc)
else()
  message(FATAL_ERROR "deps/quirc submodule missing. Run: git submodule update --init --recursive")
endif()

add_subdirectory(src/Platform)
add_subdirectory(src/Network/Relay)
add_subdirectory(tools/offs-ca)
add_subdirectory(tools/offs-release-sign)
add_subdirectory(test)
add_subdirectory(examples)

# ---------------------------------------------------------------------------
# Install / packaging
# ---------------------------------------------------------------------------
# Public client API headers — the installed surface of liboffs. Keep this
# list explicit (not a glob) so an accidental header addition does not become
# part of the installed API.
set(OFFS_PUBLIC_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c/offs_client.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c/offs_ofd_resolver.h
)

include(GNUInstallDirs)
install(TARGETS offs
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${OFFS_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/offs)

# Generate a pkg-config file so downstream consumers can do
#   pkg-config --cflags --libs offs
# The .pc.in template is configured with the install dirs and version.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/offs.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/offs.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/offs.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)