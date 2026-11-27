# The on-disk code cache with several threads and moving guest code. See Main.cpp.
add_executable(FEXCoreCacheTest Main.cpp)
target_include_directories(FEXCoreCacheTest PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreCacheTest PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)
