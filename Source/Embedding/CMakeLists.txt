# fexmac: the C embedding interface for the translator core on macOS. See
# include/fexmac.h and docs/macOS-embedding-interface.md.
#
# Three forms of the same library:
#  - fexmac: a static library for targets in this build. Its dependencies (the core and
#    the third-party libraries it uses) are linked into whatever links it, by CMake.
#  - libfexmac_bundle.a (target fexmac_bundle): a single archive for projects outside
#    this build. It holds one prelinked object in which only the fexmac_* functions are
#    global; everything else, the core and its fmt, xxhash and so on, is made private,
#    so it cannot clash with the embedder's own copies. Linking it needs only libSystem
#    and libc++. This is the form for a program signed ad hoc for the hardened runtime:
#    such a program cannot load a separately signed dylib (library validation compares
#    Team IDs, and an ad hoc signature has none).
#  - libfexmac.dylib (target fexmac_shared): the same interface as a dylib exporting only
#    fexmac_*, for an embedder that signs it and itself with the same Team ID.

set(FEXMAC_SRCS Library.cpp Options.cpp Faults.cpp Memory.cpp)
set(FEXMAC_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")

function(fexmac_setup Target)
  target_include_directories(${Target}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/Source)
  target_link_libraries(${Target} PRIVATE FEXCore Common JemallocLibs)
  set_target_properties(${Target} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN TRUE)
endfunction()

add_library(fexmac STATIC ${FEXMAC_SRCS})
fexmac_setup(fexmac)

# The same library with the test-only hooks of fexmac_testing.h, for fexmac_hook_test only.
# The production forms above and below never contain them (checked after each is built).
add_library(fexmac_testing STATIC ${FEXMAC_SRCS})
fexmac_setup(fexmac_testing)
target_compile_definitions(fexmac_testing PRIVATE FEXMAC_TESTING=1)

set(FEXMAC_NO_HOOKS_CHECK "${CMAKE_CURRENT_SOURCE_DIR}/check_no_test_hooks.sh")

add_library(fexmac_shared SHARED ${FEXMAC_SRCS})
fexmac_setup(fexmac_shared)
set_target_properties(fexmac_shared PROPERTIES OUTPUT_NAME fexmac)
target_link_options(fexmac_shared PRIVATE "LINKER:-exported_symbols_list,${FEXMAC_EXPORTS}" "LINKER:-dead_strip")
set_property(TARGET fexmac_shared APPEND PROPERTY LINK_DEPENDS "${FEXMAC_EXPORTS}")
add_custom_command(TARGET fexmac_shared POST_BUILD
  COMMAND sh "${FEXMAC_NO_HOOKS_CHECK}" $<TARGET_FILE:fexmac_shared>
  VERBATIM)
add_custom_command(TARGET fexmac POST_BUILD
  COMMAND sh "${FEXMAC_NO_HOOKS_CHECK}" $<TARGET_FILE:fexmac>
  VERBATIM)

# The standalone archive. `ld -r` combines the library's objects with every archive the
# core needs into one object, keeping only what fexmac_* reaches and turning every other
# global symbol private (Apple's single-object prelink). The archive list matches what
# CMake links for the fexmac target; the C test below fails to link if one is missing.
set(FEXMAC_BUNDLE "${CMAKE_BINARY_DIR}/Lib/libfexmac_bundle.a")
set(FEXMAC_PRELINKED "${CMAKE_CURRENT_BINARY_DIR}/fexmac_prelinked.o")
set(FEXMAC_BUNDLE_ARCHIVES
  $<TARGET_FILE:FEXCore> $<TARGET_FILE:Common> $<TARGET_FILE:JemallocLibs> $<TARGET_FILE:FEXCore_Base>
  $<TARGET_FILE:cpp-optparse> $<TARGET_FILE:tiny-json> $<TARGET_FILE:fmt> $<TARGET_FILE:xxhash>
  $<TARGET_FILE:cephes_128bit> $<TARGET_FILE:softfloat_3e> $<TARGET_FILE:rpmalloc>)
add_custom_command(
  OUTPUT "${FEXMAC_BUNDLE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Lib"
  COMMAND ${CMAKE_CXX_COMPILER} -arch arm64 -nostdlib -r -o "${FEXMAC_PRELINKED}"
    -Wl,-force_load,$<TARGET_FILE:fexmac> ${FEXMAC_BUNDLE_ARCHIVES}
    -Wl,-exported_symbols_list,${FEXMAC_EXPORTS}
  COMMAND ${CMAKE_COMMAND} -E rm -f "${FEXMAC_BUNDLE}"
  COMMAND libtool -static -no_warning_for_no_symbols -o "${FEXMAC_BUNDLE}" "${FEXMAC_PRELINKED}"
  COMMAND sh "${FEXMAC_NO_HOOKS_CHECK}" "${FEXMAC_BUNDLE}"
  DEPENDS fexmac FEXCore Common JemallocLibs FEXCore_Base cpp-optparse tiny-json fmt xxhash cephes_128bit softfloat_3e rpmalloc
    "${FEXMAC_EXPORTS}" "${FEXMAC_NO_HOOKS_CHECK}"
  COMMENT "Prelinking the standalone fexmac archive"
  VERBATIM)
add_custom_target(fexmac_bundle ALL DEPENDS "${FEXMAC_BUNDLE}")

# Tests that use the public header only, written in C, linked the way an outside
# project would link: the standalone archive plus libc++.
add_executable(fexmac_c_test tests/CApiTest.c tests/FaultHandlingTest.c tests/VectorOrderingTest.c tests/RepStringTest.c tests/TestSupport.c tests/NoFramePointer.c)
set_source_files_properties(tests/NoFramePointer.c PROPERTIES COMPILE_OPTIONS "-fomit-frame-pointer;-O2")
set_target_properties(fexmac_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(fexmac_c_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fexmac_c_test PRIVATE -Wall -Wextra -Wshadow -Werror -pedantic)
add_dependencies(fexmac_c_test fexmac_bundle)
target_link_libraries(fexmac_c_test PRIVATE "${FEXMAC_BUNDLE}" c++)
set_property(TARGET fexmac_c_test APPEND PROPERTY LINK_DEPENDS "${FEXMAC_BUNDLE}")
target_link_libraries(fexmac_c_test PRIVATE Threads::Threads)

# The state-comparison tool driven by Scripts/macos_fault_diff.py.
add_executable(fexmac_fault_diff tests/FaultDiffTool.c)
set_target_properties(fexmac_fault_diff PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(fexmac_fault_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(fexmac_fault_diff PRIVATE -Wall -Wextra -Werror)
add_dependencies(fexmac_fault_diff fexmac_bundle)
target_link_libraries(fexmac_fault_diff PRIVATE "${FEXMAC_BUNDLE}" c++)
set_property(TARGET fexmac_fault_diff APPEND PROPERTY LINK_DEPENDS "${FEXMAC_BUNDLE}")

# Tests of behaviour that needs the test-only hooks: a fault inside the library's
# handler, and where core messages go. Linked against the fexmac_testing build.
add_executable(fexmac_hook_test tests/HookTest.c tests/TestSupport.c)
set_target_properties(fexmac_hook_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_include_directories(fexmac_hook_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fexmac_hook_test PRIVATE -Wall -Wextra -Wshadow -Werror -pedantic)
target_link_libraries(fexmac_hook_test PRIVATE fexmac_testing)
