target_sources(offs PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/platform_time.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_random.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_process.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_file.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_socket.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_socket_watcher.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_thread.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_local.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_posix_compat.c
)

target_include_directories(offs PUBLIC ${CMAKE_SOURCE_DIR}/src)
