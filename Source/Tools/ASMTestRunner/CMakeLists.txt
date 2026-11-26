# Runs FEX's assembly instruction tests against the translator core alone. See Main.cpp.
add_executable(FEXCoreASMTestRunner Main.cpp)
target_include_directories(FEXCoreASMTestRunner PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreASMTestRunner PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)
