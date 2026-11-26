# Several host threads running guest code at once. See Main.cpp.
add_executable(FEXCoreThreadTest Main.cpp)
target_include_directories(FEXCoreThreadTest PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreThreadTest PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)
