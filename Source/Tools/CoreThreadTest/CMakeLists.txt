# Several host threads running guest code at once, through the fexmac interface. See
# Main.cpp. Built on the macOS embedding library only.
if (NOT APPLE)
  return()
endif()
add_executable(FEXCoreThreadTest Main.cpp)
target_link_libraries(FEXCoreThreadTest PRIVATE fexmac Threads::Threads)
