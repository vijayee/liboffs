set(CMAKE_CXX_STANDARD 17)
add_subdirectory(googletest)
add_subdirectory(BLAKE3/c)
add_subdirectory(hashmap)
option(BUILD_SHARED_LIBS "Build shared libs" OFF)
set(XXHASH_BUILD_ENABLE_INLINE_API OFF)
set(XXHASH_BUILD_XXHSUM OFF)
add_subdirectory(xxHash/cmake_unofficial EXCLUDE_FROM_ALL)
