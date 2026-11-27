# Runs FEX's assembly instruction tests against the translator core alone, through the
# fexmac interface. See Main.cpp. Common and CommonTools are linked for the harness
# only: reading test files, comparing registers and checking host features.
if (NOT APPLE)
  # The fexmac library is macOS-only, so this tool is too. No Linux target or workflow
  # uses it (the Linux core-only build never ran it).
  message(STATUS "ASMTestRunner: not built (needs the macOS-only fexmac library)")
  return()
endif()
add_executable(FEXCoreASMTestRunner Main.cpp)
target_include_directories(FEXCoreASMTestRunner PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreASMTestRunner PRIVATE fexmac FEXCore Common CommonTools)
