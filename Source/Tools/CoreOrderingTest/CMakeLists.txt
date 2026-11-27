# Multi-thread x86 memory-ordering litmus tests, through the fexmac interface. See
# Main.cpp. The fexmac library is macOS-only, so this tool is too.
add_executable(FEXCoreOrderingTest Main.cpp)
target_link_libraries(FEXCoreOrderingTest PRIVATE fexmac Threads::Threads)
