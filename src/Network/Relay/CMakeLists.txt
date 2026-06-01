add_executable(offs_relay relay_server.c relay_server_main.c)
set_target_properties(offs_relay PROPERTIES OUTPUT_NAME offs_relay)
target_include_directories(offs_relay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps
  ${CMAKE_SOURCE_DIR}/../poll-dancer/include
  ${CMAKE_SOURCE_DIR}/../poll-dancer/src
  ${CMAKE_BINARY_DIR}/deps/libcbor/include
)
target_link_libraries(offs_relay PRIVATE
  offs
  cbor
  http-parser
  ${CMAKE_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a
  ${OPENSSL_LIBRARIES}
)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/msquic/CMakeLists.txt)
  target_compile_definitions(offs_relay PRIVATE HAS_MSQUIC)
  target_include_directories(offs_relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/msquic/src/inc)
  target_link_libraries(offs_relay PRIVATE msquic::msquic msquic::platform)
endif()