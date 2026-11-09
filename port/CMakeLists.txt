# superman_returns - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(superman_returns LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Building the SDK from source (REXSDK_DIR) exposes xxHash under its own name,
# not the rex::xxhash alias the installed package exports.
if(NOT TARGET rex::xxhash AND TARGET xxhash)
    add_library(rex::xxhash ALIAS xxhash)
endif()
# The installed package ships imgui.h next to the rex headers; the source tree
# keeps it in the imgui OBJECT library, which rexruntime already links in.
if(REXSDK_DIR AND TARGET imgui)
    include_directories($<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

option(SR_VULKAN_FOUNDATION "Build standalone Vulkan foundation smoke test" OFF)
option(SR_VULKAN_GAME "Build the native Vulkan game renderer (explicit runtime selection)" OFF)
option(SR_GUEST_TEXTURE_TESTS "Build texture layout tests using SDK Xenos helpers" OFF)
if(SR_GUEST_TEXTURE_TESTS)
    add_executable(sr_guest_texture_tests ../tests/native/main.cpp ../tests/native/test_texture_layout.cpp
        src/graphics/guest/captured_batch.cpp src/graphics/guest/texture_layout.cpp src/graphics/guest/texture_capture.cpp)
    target_link_libraries(sr_guest_texture_tests PRIVATE rex::runtime)
    target_include_directories(sr_guest_texture_tests PRIVATE ../tests/native)
endif()

option(SR_LSFG "Build experimental direct-DLL LSFG presentation and GPU smoke test" OFF)
if(SR_LSFG)
    add_subdirectory(src/lsfg)
endif()
if(SR_VULKAN_FOUNDATION)
    add_subdirectory(src/graphics/vulkan)
endif()

# Sources
set(SUPERMANRETURNS_SOURCES
    src/main.cpp
    src/audio_api_log.cpp
    src/shadow_range.cpp
    src/graphics_menu.cpp
    src/launcher_settings.cpp
    src/native_audio/sample_bank.cpp
    src/native_audio/resampler.cpp
    src/native_audio/dsp.cpp
    src/native_audio/plugins.cpp
    src/native_audio/effects.cpp
    src/native_audio/engine.cpp
    src/native_audio/snd_player.cpp
    src/native_audio/bridge.cpp
    src/native_audio/bank_source.cpp
    src/native_audio/selftest.cpp
    src/frame_rate.cpp
    src/frame_stats.cpp
    src/mem_stats.cpp
    src/xma_fixes.cpp
    src/input/native_input.cpp
    src/input/gameinput_driver.cpp
    src/input/rawinput_kbm_driver.cpp
    src/input/key_capture.cpp
    src/input/input_recorder.cpp
    src/audio_out/xaudio2_output.cpp
    src/sr_settings.cpp
    src/graphics/backend_selection.cpp
    src/sr_preset.cpp
    src/skip_intro.cpp
    src/flight_tuning.cpp
    src/daynight.cpp
    src/prop_streaming.cpp
    src/world_streamer.cpp
  src/water_physics.cpp
    src/renderdoc_capture.cpp
    src/render_scale.cpp
    src/lsfg_settings.cpp
)

if(WIN32)
    add_executable(superman_returns WIN32 ${SUPERMANRETURNS_SOURCES})
else()
    add_executable(superman_returns ${SUPERMANRETURNS_SOURCES})
endif()

rexglue_setup_target(superman_returns GPU_PLUGINS xenos)
if(WIN32)
    # Native input: GameInput (Windows SDK gameinput.lib) for controllers.
    target_link_libraries(superman_returns PRIVATE gameinput xaudio2 comctl32)
endif()

# Application icon (resources/superman_returns.ico, local and optional). It is the
# first RT_GROUP_ICON of the executable, so Explorer shows it for the file and
# SDL3 uses it for the game window and the taskbar.
set(SR_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/superman_returns.ico")
if(WIN32 AND EXISTS "${SR_APP_ICON}")
    enable_language(RC)
    set(SR_APP_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/superman_returns_icon.rc")
    file(WRITE "${SR_APP_ICON_RC}" "1 ICON \"${SR_APP_ICON}\"\n")
    set_source_files_properties("${SR_APP_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${SR_APP_ICON}")
    target_sources(superman_returns PRIVATE "${SR_APP_ICON_RC}")
endif()

if(SR_LSFG)
    # Instantiate the project presenter explicitly: rexruntime is a DLL and
    # its internal provider factory would otherwise create the SDK presenter.
    target_sources(superman_returns PRIVATE src/lsfg/presentation.cpp
        third_party/rexui-lsfg/d3d12_presenter.cpp)
    target_include_directories(superman_returns PRIVATE src/lsfg third_party/rexui-lsfg)
    target_link_libraries(superman_returns PRIVATE sr_lsfg_interop d3dcompiler)
    target_compile_definitions(superman_returns PRIVATE SR_HAS_LSFG=1)
else()
    target_compile_definitions(superman_returns PRIVATE SR_HAS_LSFG=0)
endif()

# Native renderer ported from rexglue-native-kit (docs/native-port-plan.md):
#   OFF      (default) nothing native is built; the executable is unchanged.
#   CAPTURE  hooks whose address is confirmed in game_profile.h observe only.
#   RENDERER --sr_renderer=native works; needs every address confirmed
#            (static_assert) and the in-process GPU sources below.
set(SR_NATIVE "OFF" CACHE STRING "Native renderer build level: OFF, CAPTURE or RENDERER")
if(SR_VULKAN_GAME AND (NOT SR_VULKAN_FOUNDATION OR NOT SR_NATIVE STREQUAL "RENDERER"))
    message(FATAL_ERROR "SR_VULKAN_GAME requires SR_VULKAN_FOUNDATION=ON and SR_NATIVE=RENDERER")
endif()
set_property(CACHE SR_NATIVE PROPERTY STRINGS OFF CAPTURE RENDERER)
if(NOT SR_NATIVE MATCHES "^(OFF|CAPTURE|RENDERER)$")
    message(FATAL_ERROR "SR_NATIVE must be OFF, CAPTURE or RENDERER (got '${SR_NATIVE}')")
endif()
# Loose DXIL corpus packed into the executable in RENDERER builds
# (tools/shaders/build_corpus.ps1); without it use --sr_native_shader_dir.
set(SR_NATIVE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/shaders/dxil"
    CACHE PATH "DXIL shader corpus embedded by SR_NATIVE=RENDERER builds")
# OFF for release builds: nothing translated from the game is embedded or built
# next to the executable; the shaders are translated on the user's machine
# (shader_translator.h).
option(SR_NATIVE_EMBED_SHADERS "Embed the DXIL corpus and build the pre-shader library" ON)

# Compile the SDK's D3D12 GPU source in-process so a project-owned command
# processor can observe each draw. The normal executable remains available
# without the optional source checkout and keeps using the Xenos plugin.
set(SR_REXGPU_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/rexglue-sdk-source"
    CACHE PATH "ReXGlue v0.10.0 source checkout for the trace graphics system")
if(WIN32 AND EXISTS "${SR_REXGPU_SOURCE_DIR}/src/graphics/d3d12/command_processor.cpp")
    execute_process(
        COMMAND git -C "${SR_REXGPU_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE SR_REXGPU_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SR_REXGPU_GIT_RESULT
    )
    if(NOT SR_REXGPU_GIT_RESULT EQUAL 0 OR
       NOT SR_REXGPU_REVISION STREQUAL "f5337cdc947ff6d4c4196737e2c807a48f2a1fc2")
        message(FATAL_ERROR "Trace GPU sources must be ReXGlue v0.10.0 (f5337cd). Run tools/setup_gpu_source.ps1")
    endif()
    set(SR_GPU_ROOT "${SR_REXGPU_SOURCE_DIR}/src/graphics")
    set(SR_GRAPHICS_SYSTEM_SOURCE "${SR_GPU_ROOT}/graphics_system.cpp")
    if(SR_LSFG)
        set(SR_GRAPHICS_SYSTEM_SOURCE third_party/rexui-lsfg/graphics_system.cpp)
    endif()
    set(SR_GPU_SOURCES
        ${SR_GPU_ROOT}/flags.cpp
        ${SR_GPU_ROOT}/register_file.cpp
        ${SR_GPU_ROOT}/registers.cpp
        ${SR_GPU_ROOT}/pipeline/shader/shader.cpp
        ${SR_GPU_ROOT}/format/ucode.cpp
        ${SR_GPU_ROOT}/sampler_info.cpp
        ${SR_GRAPHICS_SYSTEM_SOURCE}
        ${SR_GPU_ROOT}/command_processor.cpp
        ${SR_GPU_ROOT}/util/draw_extent_estimator.cpp
        ${SR_GPU_ROOT}/util/draw.cpp
        ${SR_GPU_ROOT}/packet_disassembler.cpp
        ${SR_GPU_ROOT}/primitive_processor.cpp
        ${SR_GPU_ROOT}/pipeline/render_target/cache.cpp
        ${SR_GPU_ROOT}/shared_memory.cpp
        ${SR_GPU_ROOT}/pipeline/texture/cache.cpp
        ${SR_GPU_ROOT}/pipeline/shader/interpreter.cpp
        ${SR_GPU_ROOT}/pipeline/shader/translator.cpp
        ${SR_GPU_ROOT}/pipeline/shader/translator_disasm.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc_translator.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc_translator_alu.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc_translator_fetch.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc_translator_memexport.cpp
        ${SR_GPU_ROOT}/pipeline/shader/dxbc_translator_om.cpp
        ${SR_GPU_ROOT}/d3d12/graphics_system.cpp
        ${SR_GPU_ROOT}/d3d12/command_processor.cpp
        ${SR_GPU_ROOT}/d3d12/primitive_processor.cpp
        ${SR_GPU_ROOT}/d3d12/render_target_cache.cpp
        ${SR_GPU_ROOT}/d3d12/shader.cpp
        ${SR_GPU_ROOT}/d3d12/shared_memory.cpp
        ${SR_GPU_ROOT}/d3d12/texture_cache.cpp
        ${SR_GPU_ROOT}/d3d12/deferred_command_list.cpp
        ${SR_GPU_ROOT}/d3d12/pipeline_cache.cpp
        ${SR_REXGPU_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
        src/native_renderer/sr_graphics_system.cpp
    )
    add_library(sr_gpu STATIC ${SR_GPU_SOURCES})
    target_include_directories(sr_gpu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lsfg
        ${SR_REXGPU_SOURCE_DIR}
        ${SR_REXGPU_SOURCE_DIR}/thirdparty/renderdoc
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sr_gpu PRIVATE -Wno-unused-variable -mssse3 -msse4.1)
    endif()
    target_link_libraries(sr_gpu PRIVATE rex::runtime rex::xxhash d3d12 dxgi dxguid)
    target_link_libraries(superman_returns PRIVATE sr_gpu)
    target_compile_definitions(superman_returns PRIVATE SR_HAS_TRACE_GPU=1)

    if(NOT SR_NATIVE STREQUAL "OFF")
        # An OBJECT library, not a static one: the hook objects only override
        # weak recompiled functions, so nothing would pull them from an archive.
        set(SR_NATIVE_ROOT src/native_renderer)
        add_library(sr_native OBJECT
            ${SR_NATIVE_ROOT}/native_bridge.cpp
            ${SR_NATIVE_ROOT}/native_hooks.cpp
            ${SR_NATIVE_ROOT}/guest_cpu_profile.cpp
            ${SR_NATIVE_ROOT}/native_graphics_system.cpp
            ${SR_NATIVE_ROOT}/native_renderer.cpp
            ${SR_NATIVE_ROOT}/rt_shadows.cpp
            ${SR_NATIVE_ROOT}/water.cpp
            ${SR_NATIVE_ROOT}/dlss.cpp
            ${SR_NATIVE_ROOT}/frame_pacing.cpp
            ${SR_NATIVE_ROOT}/buffer_stock.cpp
            ${SR_NATIVE_ROOT}/precompile_screen.cpp
            ${SR_NATIVE_ROOT}/pm4_mirror.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/draw_state.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/captured_batch.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/pm4_capture.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/texture_layout.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/texture_capture.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/render_state.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/edram_alias.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/vertex_layout.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/primitive_expansion.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/render_packet.cpp
            ${SR_NATIVE_ROOT}/../graphics/guest/constant_snapshot.cpp
            ${SR_NATIVE_ROOT}/texture_decode.cpp
            ${SR_NATIVE_ROOT}/texture_replace.cpp
            ${SR_NATIVE_ROOT}/shader_registry.cpp
            ${SR_NATIVE_ROOT}/shader_translator.cpp
            ${SR_NATIVE_ROOT}/pipeline_cache.cpp
            ${SR_NATIVE_ROOT}/hang_watchdog.cpp
            ${SR_NATIVE_ROOT}/xxh3_avx2.cpp
        )
        set(SR_NATIVE_DEFINITIONS SR_HAS_NATIVE=1)
        if(SR_NATIVE STREQUAL "RENDERER")
            list(APPEND SR_NATIVE_DEFINITIONS SR_NATIVE_RENDERER_BUILD=1)
        endif()
        target_compile_definitions(sr_native PUBLIC ${SR_NATIVE_DEFINITIONS})
        target_compile_definitions(sr_native PRIVATE SR_HAS_LSFG=$<BOOL:${SR_LSFG}>)
        target_compile_definitions(sr_native PRIVATE SR_VULKAN_GAME=$<BOOL:${SR_VULKAN_GAME}>)
        target_include_directories(sr_native PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/lsfg
            ${SR_NATIVE_ROOT}
            ${SR_REXGPU_SOURCE_DIR}
            ${SR_REXGPU_SOURCE_DIR}/thirdparty/renderdoc
        )
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(sr_native PRIVATE -Wno-unused-variable -mssse3 -msse4.1)
        endif()
        # The only file built with AVX2: selected at run time (cpu_features.h), never called without it.
        set_source_files_properties(${SR_NATIVE_ROOT}/xxh3_avx2.cpp PROPERTIES
            COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/arch:AVX2,-mavx2>")
        target_link_libraries(sr_native PRIVATE rex::runtime rex::xxhash)
        if(SR_VULKAN_GAME)
            target_sources(sr_native PRIVATE src/graphics/vulkan/platform/native_provider.cpp src/graphics/vulkan/platform/shader_process.cpp)
            target_link_libraries(sr_native PRIVATE sr_vulkan_core)
            target_compile_definitions(sr_native PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
            target_compile_definitions(sr_native PRIVATE SR_VULKAN_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
            target_link_libraries(superman_returns PRIVATE sr_vulkan_core)
        endif()
        target_link_libraries(superman_returns PRIVATE sr_native d3d12 dxgi dbghelp)
        target_compile_definitions(superman_returns PRIVATE ${SR_NATIVE_DEFINITIONS})

        # NVIDIA DLSS (NGX SDK, .tools/dlss-sdk: tag v310.9.1 of github.com/NVIDIA/DLSS).
        # nvngx_dlss.dll is loaded at run time from the executable's folder.
        set(SR_DLSS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/dlss-sdk")
        target_include_directories(sr_native PRIVATE "${SR_DLSS_SDK}/include")
        target_link_libraries(superman_returns PRIVATE "${SR_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        add_custom_command(TARGET superman_returns POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SR_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:superman_returns>")

        # Offline shaders packed into one blob (RCDATA 2), as in the kit.
        file(GLOB SR_NATIVE_SHADER_FILES "${SR_NATIVE_SHADER_DIR}/*.dxil")
        if(NOT SR_NATIVE_EMBED_SHADERS)
            set(SR_NATIVE_SHADER_FILES "")
        endif()
        if(SR_NATIVE STREQUAL "RENDERER" AND SR_NATIVE_SHADER_FILES)
            enable_language(RC)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(SR_NATIVE_SHADER_PAK "${CMAKE_CURRENT_BINARY_DIR}/superman_returns_shaders.pak")
            add_custom_command(
                OUTPUT "${SR_NATIVE_SHADER_PAK}"
                COMMAND Python3::Interpreter
                        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/pack_shaders.py"
                        "${SR_NATIVE_SHADER_DIR}" "${SR_NATIVE_SHADER_PAK}"
                DEPENDS ${SR_NATIVE_SHADER_FILES}
                        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/pack_shaders.py"
                COMMENT "Packing native renderer shaders")
            set(SR_NATIVE_SHADERS_RC "${CMAKE_CURRENT_BINARY_DIR}/superman_returns_shaders.rc")
            file(WRITE "${SR_NATIVE_SHADERS_RC}" "2 RCDATA \"${SR_NATIVE_SHADER_PAK}\"\n")
            # Recorded pipelines precompiled at the first start (RCDATA 3,
            # tools/shaders/build_pipeline_db.py merges recordings into it).
            set(SR_NATIVE_RC_DEPENDS "${SR_NATIVE_SHADER_PAK}")
            set(SR_NATIVE_PIPELINE_DB "${CMAKE_CURRENT_SOURCE_DIR}/resources/superman_returns_pipelines.bin")
            if(EXISTS "${SR_NATIVE_PIPELINE_DB}")
                file(APPEND "${SR_NATIVE_SHADERS_RC}" "3 RCDATA \"${SR_NATIVE_PIPELINE_DB}\"\n")
                list(APPEND SR_NATIVE_RC_DEPENDS "${SR_NATIVE_PIPELINE_DB}")
            endif()
            set_source_files_properties("${SR_NATIVE_SHADERS_RC}" PROPERTIES
                OBJECT_DEPENDS "${SR_NATIVE_RC_DEPENDS}")
            target_sources(superman_returns PRIVATE "${SR_NATIVE_SHADERS_RC}")
        elseif(SR_NATIVE STREQUAL "RENDERER")
            message(STATUS "No DXIL in ${SR_NATIVE_SHADER_DIR}: the native renderer will "
                           "need --sr_native_shader_dir")
        endif()

        # Pre-shader library next to the executable (sr_native_preshaders): the
        # original containers + their DXIL, how the renderer recognises shaders.
        get_filename_component(SR_NATIVE_CORPUS_DIR "${SR_NATIVE_SHADER_DIR}" DIRECTORY)
        file(GLOB SR_NATIVE_RAW_FILES "${SR_NATIVE_CORPUS_DIR}/raw/*.bin")
        if(NOT SR_NATIVE_EMBED_SHADERS)
            set(SR_NATIVE_RAW_FILES "")
        endif()
        if(SR_NATIVE STREQUAL "RENDERER" AND SR_NATIVE_SHADER_FILES AND SR_NATIVE_RAW_FILES)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(SR_NATIVE_PRESHADERS "${CMAKE_CURRENT_BINARY_DIR}/superman_returns_shaders.srsl")
            add_custom_command(
                OUTPUT "${SR_NATIVE_PRESHADERS}"
                COMMAND Python3::Interpreter
                        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/make_preshaders.py"
                        --corpus "${SR_NATIVE_CORPUS_DIR}" --out "${SR_NATIVE_PRESHADERS}"
                DEPENDS ${SR_NATIVE_SHADER_FILES} ${SR_NATIVE_RAW_FILES}
                        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/make_preshaders.py"
                COMMENT "Building the pre-shader library")
            add_custom_target(sr_preshaders ALL DEPENDS "${SR_NATIVE_PRESHADERS}")
            add_dependencies(superman_returns sr_preshaders)
        endif()
    else()
        target_compile_definitions(superman_returns PRIVATE SR_HAS_NATIVE=0)
    endif()
else()
    if(SR_LSFG)
        message(FATAL_ERROR "SR_LSFG needs the ReXGlue GPU sources: run tools/setup_gpu_source.ps1")
    endif()
    if(NOT SR_NATIVE STREQUAL "OFF")
        message(FATAL_ERROR "SR_NATIVE=${SR_NATIVE} needs the ReXGlue GPU sources: run "
                            "tools/setup_gpu_source.ps1")
    endif()
    target_compile_definitions(superman_returns PRIVATE SR_HAS_TRACE_GPU=0)
    target_compile_definitions(superman_returns PRIVATE SR_HAS_NATIVE=0)
endif()

# Experimental Release build for profiling cross-file guest/native call costs.
# Keep the ordinary instruction and floating-point settings unchanged.
# Linker map next to the exe: the stack sampler (hang_watchdog.cpp) prints renderer
# frames as exe offsets, resolved against this map (the Release build has no PDB).
option(SR_LINK_MAP "Write superman_returns.map with the linker" OFF)
if(SR_LINK_MAP AND WIN32)
    target_link_options(superman_returns PRIVATE "-Wl,/MAP:${CMAKE_CURRENT_BINARY_DIR}/superman_returns.map")
endif()

option(SR_THIN_LTO "Enable Clang ThinLTO for Release guest/native/Vulkan code" OFF)
if(SR_THIN_LTO)
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "SR_THIN_LTO currently requires Windows Clang with LLD")
    endif()
    foreach(sr_lto_target IN ITEMS superman_returns superman_returns_recomp sr_native sr_vulkan_core)
        if(TARGET ${sr_lto_target})
            target_compile_options(${sr_lto_target} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
        endif()
    endforeach()
    target_link_options(superman_returns PRIVATE
        $<$<CONFIG:Release>:-flto=thin>
        $<$<CONFIG:Release>:-Wl,/opt:lldltojobs=2>
        "$<$<CONFIG:Release>:-Wl,/lldltocache:${CMAKE_CURRENT_BINARY_DIR}/thin-lto-cache>")
endif()
