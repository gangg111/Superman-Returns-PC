# Unit tests of the native audio engine's pure parts (no SDK, no game run).
#   cmake -S tests/native_audio -B build/tests-native-audio -G Ninja
#   cmake --build build/tests-native-audio && build/tests-native-audio/sr_native_audio_tests
# Tests that need the converted sounds read SR_NATIVE_AUDIO_DIR and are skipped without it.
cmake_minimum_required(VERSION 3.20)
project(sr_native_audio_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SR_AUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/native_audio")
add_executable(sr_native_audio_tests
    main.cpp
    test_sample_bank.cpp
    test_bank_source.cpp
    "${SR_AUDIO_DIR}/sample_bank.cpp"
    "${SR_AUDIO_DIR}/bank_source.cpp"
)
target_include_directories(sr_native_audio_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../native" "${SR_AUDIO_DIR}/..")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sr_native_audio_tests PRIVATE -Wall -Wextra)
endif()
