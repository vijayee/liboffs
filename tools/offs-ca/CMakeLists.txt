cmake_minimum_required(VERSION 3.22)
project(offs-ca C)

set(CMAKE_C_STANDARD 11)

find_package(OpenSSL REQUIRED)

add_executable(offs-ca
  main.c
  ca_ops.c
)

target_include_directories(offs-ca PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(offs-ca PRIVATE ${OPENSSL_INCLUDE_DIR})
# Link against the offs library so the platform_posix_compat.c helpers
# (strndup, mkdtemp, etc.) are available to the tool.
add_dependencies(offs-ca offs)
target_link_libraries(offs-ca PRIVATE offs)
target_link_libraries(offs-ca PRIVATE ${OPENSSL_LIBRARIES})

# The /experimental:c11atomics flag is set globally at the top-level
# CMakeLists.txt and propagates here through add_compile_options.
