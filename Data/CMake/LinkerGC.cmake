# SPDX-License-Identifier: MIT

# This applies some common linker options that reduce code size and linking time in Release mode. Namely:
# --gc-sections: Linktime garbage collection, discards unused sections from the final output
# --strip-all  : Similar to running `strip`, discards the symbol table from the final output
# --as-needed  : Only includes libraries that are actually needed in the final output

#
# Apple's linker does not accept those spellings. Its own options are:
# -dead_strip        : discards unreachable code and data
# -x                 : drops local symbols, keeping the exported ones a library needs
# -dead_strip_dylibs : drops libraries that nothing references

macro(LinkerGC target)
  if (CMAKE_BUILD_TYPE MATCHES "RELEASE")
    if (APPLE)
      target_link_options(${target} PRIVATE
        "LINKER:-dead_strip"
        "LINKER:-x"
        "LINKER:-dead_strip_dylibs")
    else()
      target_link_options(${target} PRIVATE
        "LINKER:--gc-sections"
        "LINKER:--strip-all"
        "LINKER:--as-needed")
    endif()
  endif()
endmacro()
