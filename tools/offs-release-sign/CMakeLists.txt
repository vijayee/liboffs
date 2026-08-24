cmake_minimum_required(VERSION 3.22)
project(offs-release-sign C)

set(CMAKE_C_STANDARD 11)

find_package(OpenSSL REQUIRED)

add_executable(offs-release-sign
  main.c
  sign_ops.c
)

target_include_directories(offs-release-sign PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(offs-release-sign PRIVATE ${OPENSSL_INCLUDE_DIR})
# Link against the offs library so the platform_posix_compat.c helpers
# (strndup, mkdtemp, etc.) are available to the tool, matching tools/offs-ca.
add_dependencies(offs-release-sign offs)
target_link_libraries(offs-release-sign PRIVATE offs)
target_link_libraries(offs-release-sign PRIVATE ${OPENSSL_LIBRARIES})

# The /experimental:c11atomics flag is set globally at the top-level
# CMakeLists.txt and propagates here through add_compile_options.