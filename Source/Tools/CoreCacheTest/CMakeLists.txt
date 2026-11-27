# The on-disk code cache with several threads and moving guest code, through the fexmac
# interface and its test-only hooks (so it links the fexmac_testing build). See Main.cpp.
if (NOT APPLE)
  message(STATUS "CoreCacheTest: not built (needs the macOS-only fexmac library)")
  return()
endif()
add_executable(FEXCoreCacheTest Main.cpp)
target_include_directories(FEXCoreCacheTest PRIVATE ${CMAKE_SOURCE_DIR}/Source/Embedding)
target_link_libraries(FEXCoreCacheTest PRIVATE fexmac_testing Threads::Threads)
