# Smallest end-to-end check of the translator core, through the fexmac interface. See
# Main.cpp. Built on the macOS embedding library only.
if (NOT APPLE)
  # The fexmac library is macOS-only, so this tool is too. No Linux target or workflow
  # uses it (the Linux core-only build never ran it).
  message(STATUS "CoreSmokeTest: not built (needs the macOS-only fexmac library)")
  return()
endif()
add_executable(FEXCoreSmokeTest Main.cpp)
target_link_libraries(FEXCoreSmokeTest PRIVATE fexmac)
