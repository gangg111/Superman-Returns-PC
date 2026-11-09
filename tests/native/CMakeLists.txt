# Unit tests of the native renderer's pure parts: they need neither the game
# nor the ReXGlue SDK, so they build on Windows, Linux or macOS.
#   cmake -S tests/native -B build/tests-native && cmake --build build/tests-native
#   ctest --test-dir build/tests-native --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(sr_native_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SR_NATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/native_renderer")
add_executable(sr_native_tests
    main.cpp
    test_backend_selection.cpp
    test_checked_guest_memory.cpp
    test_idle_yield.cpp
    test_frame_generation_policy.cpp
    test_guest_hash.cpp
    test_shared_constants_cache.cpp
    test_range_allocator.cpp
    test_texture_content.cpp
    test_texture_capture_cache.cpp
    test_texture_ranges.cpp
    test_texture_hash_replay.cpp
    test_texture_source_probe.cpp
    test_resource_unlock_audit.cpp
    test_frame_timeline.cpp
    "${SR_NATIVE_DIR}/../graphics/backend_selection.cpp"
    test_shader_requirements.cpp
    test_guest_state.cpp
    test_render_packets.cpp
    test_render_state.cpp
    test_vertex_layout.cpp
    test_primitive_expansion.cpp
    "${SR_NATIVE_DIR}/../graphics/guest/primitive_expansion.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/vertex_layout.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/edram_alias.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_packet.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/captured_batch.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/pm4_capture.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/draw_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/constant_snapshot.cpp"
    test_texture.cpp
    test_pm4_mirror.cpp
    test_shaders.cpp
    test_game_profile.cpp
    test_index_endian.cpp
    test_post_effects.cpp
    test_shader_library.cpp
    "${SR_NATIVE_DIR}/pm4_mirror.cpp"
)
target_include_directories(sr_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SR_NATIVE_DIR}")
if(WIN32)
    target_sources(sr_native_tests PRIVATE test_replay_storage_writes.cpp)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sr_native_tests PRIVATE -Wall -Wextra)
endif()

# XXH3 compiled with AVX2 (the only file built with -mavx2) against the baseline build. Needs the SDK's xxhash.h
# (.tools/rexglue-sdk); without it these tests are left out.
set(SR_XXHASH_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../.tools/rexglue-sdk/win-amd64/include")
if(WIN32 AND EXISTS "${SR_XXHASH_INCLUDE}/xxhash.h")
    target_sources(sr_native_tests PRIVATE test_xxh3_avx2.cpp xxh3_reference.cpp "${SR_NATIVE_DIR}/xxh3_avx2.cpp")
    set_source_files_properties("${SR_NATIVE_DIR}/xxh3_avx2.cpp" PROPERTIES
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/arch:AVX2,-mavx2>")
    set_source_files_properties(xxh3_reference.cpp "${SR_NATIVE_DIR}/xxh3_avx2.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${SR_XXHASH_INCLUDE}")
else()
    message(STATUS "xxhash.h not found: the AVX2 XXH3 tests are left out")
endif()

enable_testing()
add_test(NAME sr_native_tests COMMAND sr_native_tests)

add_executable(sr_capture_budget test_capture_budget.cpp
    "${SR_NATIVE_DIR}/../graphics/guest/pm4_capture.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_packet.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/captured_batch.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/constant_snapshot.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/draw_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/vertex_layout.cpp"
    "${SR_NATIVE_DIR}/pm4_mirror.cpp")
add_test(NAME sr_capture_budget COMMAND sr_capture_budget)

# Manual Release performance diagnostic; timing budgets do not belong in CTest.
add_executable(sr_replay_storage_bench replay_storage_benchmark.cpp
    "${SR_NATIVE_DIR}/../graphics/guest/pm4_capture.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_packet.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/captured_batch.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/render_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/constant_snapshot.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/draw_state.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/vertex_layout.cpp"
    "${SR_NATIVE_DIR}/pm4_mirror.cpp")
add_executable(sr_pm4_budget test_pm4_budget.cpp
    "${SR_NATIVE_DIR}/pm4_mirror.cpp"
    "${SR_NATIVE_DIR}/../graphics/guest/constant_snapshot.cpp")
