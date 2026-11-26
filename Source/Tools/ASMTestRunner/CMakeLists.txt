# Runs FEX's assembly instruction tests against the translator core alone. See Main.cpp.
add_executable(FEXCoreASMTestRunner Main.cpp)
target_include_directories(FEXCoreASMTestRunner PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools)
target_link_libraries(FEXCoreASMTestRunner PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)

if (APPLE)
  # The tests place guest code and data at fixed addresses below 4GB. macOS reserves
  # that whole range by default (the "page zero" segment) and loads the program just
  # above it. Shrink the reservation to one page and move the program well out of the
  # way. There is no run-time call that frees page zero, so this must be a link option.
  target_link_options(FEXCoreASMTestRunner PRIVATE
    LINKER:-pagezero_size,0x4000
    LINKER:-image_base,0x400000000000)
endif()
