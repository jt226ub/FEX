# Smallest end-to-end check of the translator core. See Main.cpp.
add_executable(FEXCoreSmokeTest Main.cpp)
target_include_directories(FEXCoreSmokeTest PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/Source)
target_link_libraries(FEXCoreSmokeTest PRIVATE FEXCore Common CommonTools JemallocLibs Threads::Threads)
