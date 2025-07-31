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
            test_time.cpp
            test_section.cpp)
    add_dependencies(testliboffs cbor)
    add_dependencies(testliboffs offs)
    add_dependencies(testliboffs blake3)
    target_link_libraries(testliboffs PRIVATE offs)
    target_link_libraries(testliboffs PRIVATE blake3)
    target_link_libraries(testliboffs PRIVATE hashmap)
    target_link_libraries(testliboffs PUBLIC cbor)
    target_link_libraries(testliboffs PRIVATE GTest::gtest_main)
    target_link_libraries(testliboffs PRIVATE GTest::gmock)
    target_include_directories(testliboffs PUBLIC ${C_INC})

    include(GoogleTest)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Finds all the Google tests associated with the executable
    gtest_discover_tests(testliboffs)
endif()