# Loads a freestanding x86-64 program (the benchmark in unittests/macOS/Bench) and runs
# it through the fexmac interface. See Main.cpp and docs/macOS-benchmarks.md.
if (NOT APPLE)
  message(STATUS "CoreBench: not built (needs the macOS-only fexmac library)")
  return()
endif()
add_executable(FEXCoreBench Main.cpp)
target_link_libraries(FEXCoreBench PRIVATE fexmac Threads::Threads)
