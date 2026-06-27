project(examples C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# off_server: standalone C HTTP server
add_executable(off_server off_server/main.c)
add_dependencies(off_server cbor)
add_dependencies(off_server offs)
add_dependencies(off_server blake3)
add_dependencies(off_server http-parser)
target_link_libraries(off_server PRIVATE offs)
target_link_libraries(off_server PRIVATE blake3)
target_link_libraries(off_server PRIVATE hashmap)
target_link_libraries(off_server PRIVATE http-parser)
target_link_libraries(off_server PUBLIC cbor)
target_link_libraries(off_server PRIVATE poll-dancer)
target_link_libraries(off_server PRIVATE ${OPENSSL_LIBRARIES})
if(NOT WIN32)
  target_link_libraries(off_server PRIVATE pthread)
  target_link_libraries(off_server PRIVATE m)
endif()
target_include_directories(off_server PUBLIC ${C_INC})
target_include_directories(off_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories(off_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
target_include_directories(off_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
# msquic builds as a shared library (msquic.dll, via QUIC_BUILD_SHARED). Copy it
# next to the off_server executable so the matching version is found at load time:
# a stale msquic.dll elsewhere on PATH (e.g. the Windows Performance Toolkit
# copy, v1.9.x) would otherwise be loaded and MsQuicOpen2 would fail with
# E_NOINTERFACE (0x80004002, version mismatch), leaving the WT/QUIC transport
# unavailable even though HTTP still serves. copy_if_different keeps this a
# no-op on incremental builds. Mirrors test/CMakeLists.txt for testliboffs.
if(OFFS_ENABLE_QUIC AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
  add_custom_command(TARGET off_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic::msquic> $<TARGET_FILE_DIR:off_server>)
endif()