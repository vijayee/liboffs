if (BUILD_TESTING)
    project(testliboffs  C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    # Build googletest if not already found
    if (NOT TARGET GTest::gtest_main)
      set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest ${CMAKE_BINARY_DIR}/deps/googletest)
    endif()

    set(GTEST_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest/googletest/include)
    set(GMOCK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest/googlemock/include)

    message(STATUS "${C_INC}")
    add_executable(testliboffs test_main.cpp
            test_buffer.cpp
            test_refcounter.cpp
            test_block.cpp
            test_index.cpp
            test_section.cpp
            test_block_cache.cpp
            test_file_stream.cpp
            test_message_queue.cpp
            test_actor.cpp
            test_pool.cpp
            test_deque.cpp
            test_scheduler.cpp
            test_timer_actor.cpp
            test_stream.cpp
            test_tuple_ori.cpp
            test_tuple_cache.cpp
            test_readable_off_stream.cpp
            test_writeable_off_stream.cpp
            test_readable_descriptor.cpp
            test_writeable_descriptor.cpp
            test_off_stream_integration.cpp
            test_block_recipe.cpp
            test_http_server.cpp
            test_off_url.cpp
            test_ofd.cpp
            test_ofd_cache.cpp
            test_off_routes.cpp
            test_network.cpp
            test_wanted_list.cpp
            test_stream_network.cpp
            test_stream_framer.cpp
            test_nat_detect.cpp
            test_conn_state.cpp
            test_quic_integration.cpp
            test_unix_transport.cpp
            test_tcp_transport.cpp
            test_ws_transport.cpp
            test_offs_client.cpp
            test_peer_verify.cpp
            test_platform.cpp
            test_platform_socket.cpp
            test_platform_file_sync.cpp
            test_timer_debounce.cpp
            test_shutdown.cpp
            test_config_validate.cpp
            test_validation.cpp
            test_wire_validation.cpp
            test_bcrypt.cpp
            test_auth_middleware.cpp
            test_auth_wire.cpp
            test_recycler_wire.cpp
            test_block_cache_api.cpp
            test_health_handler.cpp
            test_health_wire.cpp
            test_health_http.cpp
            )
    add_dependencies(testliboffs cbor)
    add_dependencies(testliboffs offs)
    add_dependencies(testliboffs blake3)
    add_dependencies(testliboffs http-parser)
    target_link_libraries(testliboffs PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(testliboffs PRIVATE ssl crypto)
    target_link_libraries(testliboffs PRIVATE blake3)
    target_link_libraries(testliboffs PRIVATE hashmap)
    target_link_libraries(testliboffs PRIVATE http-parser)
    target_link_libraries(testliboffs PUBLIC cbor)
    target_link_libraries(testliboffs PRIVATE poll-dancer)
    target_link_libraries(testliboffs PRIVATE GTest::gtest_main)
    target_link_libraries(testliboffs PRIVATE GTest::gmock)
    # Must match HAS_MSQUIC definition from parent CMakeLists.txt so
    # struct layouts (peer_connection_t etc.) are consistent with the library.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(testliboffs PRIVATE HAS_MSQUIC)
      target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(testliboffs PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(testliboffs PUBLIC ${C_INC})
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(testliboffs PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(testliboffs PRIVATE ${GMOCK_INCLUDE_DIR})

    include(GoogleTest)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Use gtest_add_tests instead of gtest_discover_tests because msquic init
    # errors at runtime cause discover_tests to fail during build.
    gtest_add_tests(TARGET testliboffs TEST_LIST test_list)
    set_tests_properties(${test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(test_file_transfer_integration test_file_transfer_integration.cpp test_node_main.c)
    add_dependencies(test_file_transfer_integration cbor)
    add_dependencies(test_file_transfer_integration offs)
    add_dependencies(test_file_transfer_integration blake3)
    add_dependencies(test_file_transfer_integration http-parser)
    target_link_libraries(test_file_transfer_integration PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_file_transfer_integration PRIVATE ssl crypto)
    target_link_libraries(test_file_transfer_integration PRIVATE blake3)
    target_link_libraries(test_file_transfer_integration PRIVATE hashmap)
    target_link_libraries(test_file_transfer_integration PRIVATE http-parser)
    target_link_libraries(test_file_transfer_integration PUBLIC cbor)
    target_link_libraries(test_file_transfer_integration PRIVATE poll-dancer)
    target_link_libraries(test_file_transfer_integration PRIVATE GTest::gtest_main)
    target_link_libraries(test_file_transfer_integration PRIVATE GTest::gmock)
    target_link_libraries(test_file_transfer_integration PRIVATE pthread)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(test_file_transfer_integration PRIVATE HAS_MSQUIC)
      target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(test_file_transfer_integration PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(test_file_transfer_integration PUBLIC ${C_INC})
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(test_file_transfer_integration PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(test_file_transfer_integration PRIVATE ${GMOCK_INCLUDE_DIR})
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)
    add_executable(test_offs_client_integration test_offs_client_integration.cpp)
    add_dependencies(test_offs_client_integration cbor)
    add_dependencies(test_offs_client_integration offs)
    add_dependencies(test_offs_client_integration blake3)
    add_dependencies(test_offs_client_integration http-parser)
    target_link_libraries(test_offs_client_integration PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_offs_client_integration PRIVATE ssl crypto)
    target_link_libraries(test_offs_client_integration PRIVATE blake3)
    target_link_libraries(test_offs_client_integration PRIVATE hashmap)
    target_link_libraries(test_offs_client_integration PRIVATE http-parser)
    target_link_libraries(test_offs_client_integration PUBLIC cbor)
    target_link_libraries(test_offs_client_integration PRIVATE poll-dancer)
    target_link_libraries(test_offs_client_integration PRIVATE GTest::gtest_main)
    target_link_libraries(test_offs_client_integration PRIVATE GTest::gmock)
    target_link_libraries(test_offs_client_integration PRIVATE pthread)
    target_include_directories(test_offs_client_integration PUBLIC ${C_INC})
    target_include_directories(test_offs_client_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_offs_client_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_offs_client_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(test_offs_client_integration PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(test_offs_client_integration PRIVATE ${GMOCK_INCLUDE_DIR})
    target_include_directories(test_offs_client_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)
    gtest_add_tests(TARGET test_offs_client_integration TEST_LIST offs_client_integration_test_list)
    set_tests_properties(${offs_client_integration_test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(test_rpc_integration test_rpc_integration.cpp test_node_main.c)
    add_dependencies(test_rpc_integration cbor)
    add_dependencies(test_rpc_integration offs)
    add_dependencies(test_rpc_integration blake3)
    add_dependencies(test_rpc_integration http-parser)
    target_compile_definitions(test_rpc_integration PRIVATE OFFS_TEST)
    target_link_libraries(test_rpc_integration PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_rpc_integration PRIVATE ssl crypto)
    target_link_libraries(test_rpc_integration PRIVATE blake3)
    target_link_libraries(test_rpc_integration PRIVATE hashmap)
    target_link_libraries(test_rpc_integration PRIVATE http-parser)
    target_link_libraries(test_rpc_integration PUBLIC cbor)
    target_link_libraries(test_rpc_integration PRIVATE poll-dancer)
    target_link_libraries(test_rpc_integration PRIVATE GTest::gtest_main)
    target_link_libraries(test_rpc_integration PRIVATE GTest::gmock)
    target_link_libraries(test_rpc_integration PRIVATE pthread)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(test_rpc_integration PRIVATE HAS_MSQUIC)
      target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(test_rpc_integration PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(test_rpc_integration PUBLIC ${C_INC})
    target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(test_rpc_integration PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(test_rpc_integration PRIVATE ${GMOCK_INCLUDE_DIR})
    target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)

    gtest_add_tests(TARGET test_rpc_integration TEST_LIST rpc_test_list)

    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test.pdf DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/certs DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/certs DESTINATION ${CMAKE_BINARY_DIR})

    # offs-ca tool tests
    add_executable(test_offs_ca
      test_offs_ca.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca/ca_ops.c
    )
    target_include_directories(test_offs_ca PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca)
    target_link_libraries(test_offs_ca PRIVATE ssl crypto)
    target_link_libraries(test_offs_ca PRIVATE GTest::gtest_main)
    gtest_add_tests(TARGET test_offs_ca TEST_LIST ca_test_list)

    add_executable(test_version
      test_version.cpp
    )
    target_link_libraries(test_version PRIVATE offs GTest::gtest GTest::gtest_main)
    target_include_directories(test_version PRIVATE ${C_INC} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    gtest_add_tests(TARGET test_version TEST_LIST version_test_list)

    add_executable(test_large_file_upload test_large_file_upload.cpp)
    add_dependencies(test_large_file_upload cbor)
    add_dependencies(test_large_file_upload offs)
    add_dependencies(test_large_file_upload blake3)
    add_dependencies(test_large_file_upload http-parser)
    target_link_libraries(test_large_file_upload PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_large_file_upload PRIVATE ssl crypto)
    target_link_libraries(test_large_file_upload PRIVATE blake3)
    target_link_libraries(test_large_file_upload PRIVATE hashmap)
    target_link_libraries(test_large_file_upload PRIVATE http-parser)
    target_link_libraries(test_large_file_upload PUBLIC cbor)
    target_link_libraries(test_large_file_upload PRIVATE poll-dancer)
    target_link_libraries(test_large_file_upload PRIVATE GTest::gtest_main)
    target_link_libraries(test_large_file_upload PRIVATE GTest::gmock)
    target_link_libraries(test_large_file_upload PRIVATE pthread)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(test_large_file_upload PRIVATE HAS_MSQUIC)
      target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(test_large_file_upload PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(test_large_file_upload PUBLIC ${C_INC})
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)
    target_include_directories(test_large_file_upload PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(test_large_file_upload PRIVATE ${GMOCK_INCLUDE_DIR})
    gtest_add_tests(TARGET test_large_file_upload TEST_LIST large_file_upload_test_list)
endif()