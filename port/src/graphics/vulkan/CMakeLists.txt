set(SR_VULKAN_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.tools/vulkan-headers/include" CACHE PATH "Pinned Vulkan headers")
if(NOT EXISTS "${SR_VULKAN_HEADERS}/vulkan/vulkan.h")
  message(FATAL_ERROR "Run tools/setup_vulkan.ps1 to fetch headers")
endif()
add_library(sr_vulkan_core STATIC policy.cpp loader.cpp context.cpp swapchain.cpp frame_loop.cpp triangle.cpp smoke_options.cpp device_requirements.cpp descriptors.cpp descriptor_sets.cpp resources.cpp image_state.cpp render_targets.cpp game_pipeline.cpp game_renderer.cpp ../guest/edram_alias.cpp ../shaders/vulkan_shader_service.cpp)
target_link_libraries(sr_vulkan_core PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(sr_vulkan_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${SR_VULKAN_HEADERS}")
target_compile_features(sr_vulkan_core PUBLIC cxx_std_23)
target_sources(sr_vulkan_core PRIVATE ../guest/captured_batch.cpp ../guest/primitive_expansion.cpp ../guest/vertex_layout.cpp resolve.cpp)
target_compile_definitions(sr_vulkan_core PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
  set(SR_VULKAN_DXC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.tools/dxc/bin/x64/dxc.exe" CACHE FILEPATH "DXC with SPIR-V support")
else()
  find_program(SR_VULKAN_DXC NAMES dxc REQUIRED)
endif()
set(helper_dir "${CMAKE_CURRENT_BINARY_DIR}/helpers")
file(MAKE_DIRECTORY "${helper_dir}")
add_custom_command(OUTPUT "${helper_dir}/depth_resolve.spv"
  COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E CSMain -Fo "${helper_dir}/depth_resolve.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth_resolve.hlsl"
  DEPENDS shaders/depth_resolve.hlsl VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/depth_resolve_shader.h"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/depth_resolve.spv" "-DOUTPUT=${helper_dir}/depth_resolve_shader.h" -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
  DEPENDS "${helper_dir}/depth_resolve.spv" shaders/embed_spirv.cmake VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/depth_resolve_image.spv"
  COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E CSMain -Fo "${helper_dir}/depth_resolve_image.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth_resolve_image.hlsl"
  DEPENDS shaders/depth_resolve_image.hlsl VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/depth_resolve_image_shader.h"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/depth_resolve_image.spv" "-DOUTPUT=${helper_dir}/depth_resolve_image_shader.h" -DNAME=depth_resolve_image_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
  DEPENDS "${helper_dir}/depth_resolve_image.spv" shaders/embed_spirv.cmake VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/resolve_scale_copy.spv"
  COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E CSMain -Fo "${helper_dir}/resolve_scale_copy.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/resolve_scale_copy.hlsl"
  DEPENDS shaders/resolve_scale_copy.hlsl VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/resolve_scale_copy_shader.h"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/resolve_scale_copy.spv" "-DOUTPUT=${helper_dir}/resolve_scale_copy_shader.h" -DNAME=resolve_scale_copy_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
  DEPENDS "${helper_dir}/resolve_scale_copy.spv" shaders/embed_spirv.cmake VERBATIM)
target_sources(sr_vulkan_core PRIVATE depth_resolve.cpp "${helper_dir}/depth_resolve_shader.h" "${helper_dir}/depth_resolve_image_shader.h" "${helper_dir}/resolve_scale_copy_shader.h")
target_include_directories(sr_vulkan_core PRIVATE "${helper_dir}")
add_custom_command(OUTPUT "${helper_dir}/edram_alias.spv"
  COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E CSMain -Fo "${helper_dir}/edram_alias.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/edram_alias_compute.hlsl"
  DEPENDS shaders/edram_alias_compute.hlsl ../../native_renderer/shaders/edram_alias.hlsl VERBATIM)
add_custom_command(OUTPUT "${helper_dir}/edram_alias_shader.h"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/edram_alias.spv" "-DOUTPUT=${helper_dir}/edram_alias_shader.h" -DNAME=edram_alias_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
  DEPENDS "${helper_dir}/edram_alias.spv" shaders/embed_spirv.cmake VERBATIM)
target_sources(sr_vulkan_core PRIVATE "${helper_dir}/edram_alias_shader.h")
foreach(stage IN ITEMS vs ps)
  if(stage STREQUAL "vs")
    set(composition_entry VSMain)
  else()
    set(composition_entry PSMain)
  endif()
  add_custom_command(OUTPUT "${helper_dir}/composition_${stage}.spv"
    COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T ${stage}_6_0 -E ${composition_entry} -Fo "${helper_dir}/composition_${stage}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/composition.hlsl"
    DEPENDS shaders/composition.hlsl VERBATIM)
  add_custom_command(OUTPUT "${helper_dir}/composition_${stage}.h"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/composition_${stage}.spv" "-DOUTPUT=${helper_dir}/composition_${stage}.h" "-DNAME=composition_${stage}" -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
    DEPENDS "${helper_dir}/composition_${stage}.spv" shaders/embed_spirv.cmake VERBATIM)
  target_sources(sr_vulkan_core PRIVATE "${helper_dir}/composition_${stage}.h")
endforeach()
foreach(stage IN ITEMS vs ps)
  if(stage STREQUAL "vs")
    set(ui_entry VSMain)
  else()
    set(ui_entry PSMain)
  endif()
  add_custom_command(OUTPUT "${helper_dir}/immediate_${stage}.spv"
    COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T ${stage}_6_0 -E ${ui_entry} -Fo "${helper_dir}/immediate_${stage}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/immediate.hlsl"
    DEPENDS shaders/immediate.hlsl VERBATIM)
  add_custom_command(OUTPUT "${helper_dir}/immediate_${stage}.h"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${helper_dir}/immediate_${stage}.spv" "-DOUTPUT=${helper_dir}/immediate_${stage}.h" "-DNAME=immediate_${stage}" -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
    DEPENDS "${helper_dir}/immediate_${stage}.spv" shaders/embed_spirv.cmake VERBATIM)
  target_sources(sr_vulkan_core PRIVATE "${helper_dir}/immediate_${stage}.h")
endforeach()
target_sources(sr_vulkan_core PRIVATE composition.cpp game_frame.cpp immediate.cpp)
if(TARGET sr_lsfg_backend)
  target_sources(sr_vulkan_core PRIVATE frame_generation.cpp)
  target_link_libraries(sr_vulkan_core PUBLIC sr_lsfg_backend)
  add_executable(sr_vulkan_frame_generation_test "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/vulkan/frame_generation_integration.cpp" platform/win32_surface.cpp)
  target_link_libraries(sr_vulkan_frame_generation_test PRIVATE sr_vulkan_core sr_lsfg_backend user32)
  target_compile_definitions(sr_vulkan_frame_generation_test PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
endif()
if(WIN32)
  set(SR_VULKAN_DXC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.tools/dxc/bin/x64/dxc.exe" CACHE FILEPATH "DXC with SPIR-V support")
  if(NOT EXISTS "${SR_VULKAN_DXC}")
    message(FATAL_ERROR "DXC is required to compile project-owned triangle shaders")
  endif()
  set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${shader_dir}")
  foreach(stage IN ITEMS vert frag)
    if(stage STREQUAL "vert")
      set(entry VSMain)
      set(profile vs_6_0)
    else()
      set(entry PSMain)
      set(profile ps_6_0)
    endif()
    add_custom_command(OUTPUT "${shader_dir}/triangle.${stage}.spv"
      COMMAND "${SR_VULKAN_DXC}" -spirv -fspv-target-env=vulkan1.1 -T ${profile} -E ${entry} -Fo "${shader_dir}/triangle.${stage}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.hlsl" VERBATIM)
  endforeach()
  add_executable(sr_vulkan_smoke smoke_main.cpp platform/win32_surface.cpp "${shader_dir}/triangle.vert.spv" "${shader_dir}/triangle.frag.spv")
  target_compile_definitions(sr_vulkan_smoke PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
  target_link_libraries(sr_vulkan_smoke PRIVATE sr_vulkan_core user32)
  add_custom_command(TARGET sr_vulkan_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sr_vulkan_smoke>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shader_dir}/triangle.vert.spv" "${shader_dir}/triangle.frag.spv" "$<TARGET_FILE_DIR:sr_vulkan_smoke>/shaders" VERBATIM)
endif()
