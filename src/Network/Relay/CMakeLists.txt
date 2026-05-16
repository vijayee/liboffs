add_executable(meridian_relay relay_server.c relay_server_main.c)
target_include_directories(meridian_relay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps
  ${CMAKE_SOURCE_DIR}/../poll-dancer/include
  ${CMAKE_SOURCE_DIR}/../poll-dancer/src
  ${CMAKE_BINARY_DIR}/deps/libcbor/include
)
target_link_libraries(meridian_relay PRIVATE
  offs
  cbor
  http-parser
  ${CMAKE_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a
  ${OPENSSL_LIBRARIES}
)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/msquic/CMakeLists.txt)
  target_compile_definitions(meridian_relay PRIVATE HAS_MSQUIC)
  target_include_directories(meridian_relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/msquic/src/inc)
  target_link_libraries(meridian_relay PRIVATE msquic::msquic msquic::platform)
endif()