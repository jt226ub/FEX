# Lost wake-up check for FEXCore's WritePriorityMutex. See Main.cpp.
add_executable(FEXCoreMutexWakeTest Main.cpp)
target_include_directories(FEXCoreMutexWakeTest PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(FEXCoreMutexWakeTest PRIVATE FEXCore Common JemallocLibs Threads::Threads)
