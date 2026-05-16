if (BUILD_TESTING)
    project(testliboffs  C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
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
            test_quic_integration.cpp)
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
    target_link_libraries(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/build/libpoll_dancer.a)
    target_link_libraries(testliboffs PRIVATE GTest::gtest_main)
    target_link_libraries(testliboffs PRIVATE GTest::gmock)
    # Must match HAS_MSQUIC definition from parent CMakeLists.txt so
    # struct layouts (peer_connection_t etc.) are consistent with the library.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/msquic/CMakeLists.txt)
      target_compile_definitions(testliboffs PRIVATE HAS_MSQUIC)
      target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/msquic/src/inc)
      target_link_libraries(testliboffs PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(testliboffs PUBLIC ${C_INC})
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/include)
    target_include_directories(testliboffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)

    include(GoogleTest)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Use gtest_add_tests instead of gtest_discover_tests because msquic init
    # errors at runtime cause discover_tests to fail during build.
    gtest_add_tests(TARGET testliboffs TEST_LIST test_list)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test.pdf DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()