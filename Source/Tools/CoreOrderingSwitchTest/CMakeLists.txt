# The ordering switch of fexmac's TSODEFERRED option (Main.cpp), twice: against the
# production library, and against fexmac_testing, whose hooks can break the switch on
# purpose (--break) to show the test can fail. macOS only, like fexmac.
add_executable(FEXCoreOrderingSwitchTest Main.cpp)
target_link_libraries(FEXCoreOrderingSwitchTest PRIVATE fexmac Threads::Threads)

add_executable(FEXCoreOrderingSwitchTest_hooks Main.cpp)
target_compile_definitions(FEXCoreOrderingSwitchTest_hooks PRIVATE SWITCH_TEST_HOOKS=1)
target_include_directories(FEXCoreOrderingSwitchTest_hooks PRIVATE ${CMAKE_SOURCE_DIR}/Source/Embedding)
target_link_libraries(FEXCoreOrderingSwitchTest_hooks PRIVATE fexmac_testing Threads::Threads)
