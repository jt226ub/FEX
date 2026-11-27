# Smallest end-to-end check of the translator core, through the fexmac interface. See
# Main.cpp. Built on the macOS embedding library only.
if (NOT APPLE)
  return()
endif()
add_executable(FEXCoreSmokeTest Main.cpp)
target_link_libraries(FEXCoreSmokeTest PRIVATE fexmac)
