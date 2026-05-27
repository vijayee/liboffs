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
target_link_libraries(offs-ca PRIVATE ${OPENSSL_LIBRARIES})
