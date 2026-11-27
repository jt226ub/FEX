# Multi-thread x86 memory-ordering litmus tests. See Main.cpp.
add_executable(FEXCoreOrderingTest Main.cpp)
target_include_directories(FEXCoreOrderingTest PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreOrderingTest PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)
