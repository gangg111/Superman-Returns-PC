// Ray-traced sun shadows (docs/cienie-rt-projekt.md): the DXR scene the shadow
// receivers trace with inline RayQuery. Step 2 holds a single test cube placed
// on the sun line through the centre of the game's first cascade (where the
// player is), so its shadow must land next to Superman.

#include "native_renderer.h"

#include <algorithm>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

REXCVAR_DEFINE_DOUBLE(sr_rt_test_cube, 0.0, "Superman Returns Native",
                      "Ray-traced shadows step 2: light-space depth (0..1) of a 6-unit test cube on "
                      "the sun line through the first cascade's centre; 0 = no cube")
    .range(0.0, 1.0);
REXCVAR_DEFINE_INT32(sr_rt_exclude_sources, 0, "Superman Returns Native",
                     "Debug: leave these capture sources out of the ray-traced scene (bit mask: 1 = indexed "
                     "scene draws, 2 = the shadow caster pass, 4 = instanced draws)")
    .range(0, 7);

namespace superman_returns::native {

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, bool uav) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (uav) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> buffer;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&buffer)))) {
    return nullptr;
  }
  return buffer;
}

void Fill(ID3D12Resource* buffer, const void* data, size_t size) {
  void* mapped = nullptr;
  D3D12_RANGE none{0, 0};
  if (SUCCEEDED(buffer->Map(0, &none, &mapped))) {
    std::memcpy(mapped, data, size);
    buffer->Unmap(0, nullptr);
  }
}

}  // namespace

// Builds the BLAS/TLAS (records the build on the frame's command list) and
// rebuilds them when the first cascade moved more than 50 units away from the
// cube: the light matrix of the first frames (menus, loading) is not the
// player's.
bool Renderer::EnsureRtScene(const float inv_light[16]) {
  if (rt_scene_failed_) return false;
  const float depth = float(REXCVAR_GET(sr_rt_test_cube));
  if (depth <= 0.0f) return false;
  // Cube centre: inverse light matrix applied to (0.5, 0.5, depth).
  auto world = [&](float u, float v, float z, float out[3]) {
    for (int r = 0; r < 3; ++r)
      out[r] = inv_light[r * 4] * u + inv_light[r * 4 + 1] * v + inv_light[r * 4 + 2] * z +
               inv_light[r * 4 + 3];
  };
  float c[3];
  world(0.5f, 0.5f, depth, c);
  if (rt_scene_ready_) {
    const float dx = c[0] - rt_cube_center_[0], dy = c[1] - rt_cube_center_[1],
                dz = c[2] - rt_cube_center_[2];
    if (dx * dx + dy * dy + dz * dz < 50.0f * 50.0f) return true;
    Retire(std::move(rt_cube_vb_));
    Retire(std::move(rt_cube_ib_));
    Retire(std::move(rt_blas_));
    Retire(std::move(rt_tlas_));
    Retire(std::move(rt_instances_));
    Retire(std::move(rt_scratch_));
    rt_scene_ready_ = false;
  }
  rt_scene_failed_ = true;  // cleared on success
  if ((!rt_device_ && FAILED(device_->QueryInterface(IID_PPV_ARGS(&rt_device_)))) ||
      (!rt_list_ && FAILED(command_list_.As(&rt_list_)))) {
    REXLOG_WARN("native: rt shadows: no DXR device/command list");
    return false;
  }
  std::memcpy(rt_cube_center_, c, sizeof(c));
  const float h = 3.0f;
  const float vertices[8][3] = {
      {c[0] - h, c[1] - h, c[2] - h}, {c[0] + h, c[1] - h, c[2] - h},
      {c[0] + h, c[1] + h, c[2] - h}, {c[0] - h, c[1] + h, c[2] - h},
      {c[0] - h, c[1] - h, c[2] + h}, {c[0] + h, c[1] - h, c[2] + h},
      {c[0] + h, c[1] + h, c[2] + h}, {c[0] - h, c[1] + h, c[2] + h}};
  const uint16_t indices[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                                1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
  rt_cube_vb_ = MakeBuffer(device_, sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ, false);
  rt_cube_ib_ = MakeBuffer(device_, sizeof(indices), D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ, false);
  if (!rt_cube_vb_ || !rt_cube_ib_) return false;
  Fill(rt_cube_vb_.Get(), vertices, sizeof(vertices));
  Fill(rt_cube_ib_.Get(), indices, sizeof(indices));

  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  geometry.Triangles.VertexBuffer.StartAddress = rt_cube_vb_->GetGPUVirtualAddress();
  geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
  geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geometry.Triangles.VertexCount = 8;
  geometry.Triangles.IndexBuffer = rt_cube_ib_->GetGPUVirtualAddress();
  geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
  geometry.Triangles.IndexCount = 36;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in{};
  blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  blas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  blas_in.NumDescs = 1;
  blas_in.pGeometryDescs = &geometry;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
  rt_device_->GetRaytracingAccelerationStructurePrebuildInfo(&blas_in, &blas_info);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in{};
  tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlas_in.NumDescs = 1;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
  rt_device_->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_in, &tlas_info);

  const uint64_t scratch_size =
      std::max(blas_info.ScratchDataSizeInBytes, tlas_info.ScratchDataSizeInBytes);
  rt_scratch_ = MakeBuffer(device_, scratch_size, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  rt_blas_ = MakeBuffer(device_, blas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
  rt_tlas_ = MakeBuffer(device_, tlas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
  rt_instances_ = MakeBuffer(device_, sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                             D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
  if (!rt_scratch_ || !rt_blas_ || !rt_tlas_ || !rt_instances_) return false;

  D3D12_RAYTRACING_INSTANCE_DESC instance{};
  instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
  instance.InstanceMask = 0xFF;
  instance.AccelerationStructure = rt_blas_->GetGPUVirtualAddress();
  Fill(rt_instances_.Get(), &instance, sizeof(instance));
  tlas_in.InstanceDescs = rt_instances_->GetGPUVirtualAddress();

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = blas_in;
  build.DestAccelerationStructureData = rt_blas_->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = rt_scratch_->GetGPUVirtualAddress();
  rt_list_->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = rt_blas_.Get();
  rt_list_->ResourceBarrier(1, &uav);
  build.Inputs = tlas_in;
  build.DestAccelerationStructureData = rt_tlas_->GetGPUVirtualAddress();
  rt_list_->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  uav.UAV.pResource = rt_tlas_.Get();
  rt_list_->ResourceBarrier(1, &uav);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.RaytracingAccelerationStructure.Location = rt_tlas_->GetGPUVirtualAddress();
  device_->CreateShaderResourceView(
      nullptr, &srv,
      provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), kRtTlasSrv));
  rt_scene_failed_ = false;
  rt_scene_ready_ = true;
  REXLOG_INFO("native: rt shadows: test cube at ({:.1f} {:.1f} {:.1f}), blas {} B, tlas {} B", c[0],
              c[1], c[2], blas_info.ResultDataMaxSizeInBytes, tlas_info.ResultDataMaxSizeInBytes);
  return true;
}

// One bottom-level structure for a game mesh (built on the frame's command
// list while the vertex/index buffers are current). The scratch buffer is
// shared by every build of the frame; a UAV barrier separates them.
bool Renderer::RtBuildBlas(const D3D12_RAYTRACING_GEOMETRY_DESC& geometry, RtMesh& mesh) {
  if ((!rt_device_ && FAILED(device_->QueryInterface(IID_PPV_ARGS(&rt_device_)))) ||
      (!rt_list_ && FAILED(command_list_.As(&rt_list_)))) {
    return false;
  }
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = 1;
  inputs.pGeometryDescs = &geometry;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  rt_device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  if (!info.ResultDataMaxSizeInBytes) return false;
  if (info.ScratchDataSizeInBytes > rt_build_scratch_size_) {
    if (rt_build_scratch_) Retire(std::move(rt_build_scratch_));
    rt_build_scratch_size_ = std::max<uint64_t>(info.ScratchDataSizeInBytes, 4u << 20);
    rt_build_scratch_ = MakeBuffer(device_, rt_build_scratch_size_, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (!rt_build_scratch_) {
      rt_build_scratch_size_ = 0;
      return false;
    }
  }
  mesh.blas = MakeBuffer(device_, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
  if (!mesh.blas) return false;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = inputs;
  build.DestAccelerationStructureData = mesh.blas->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = rt_build_scratch_->GetGPUVirtualAddress();
  rt_list_->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = nullptr;  // scratch reuse and the following TLAS build
  rt_list_->ResourceBarrier(1, &uav);
  return true;
}

// Index buffer a mesh owns (instanced meshes keep their indices in a vertex
// stream, read from guest memory once). Upload heap: written once, read by the
// BLAS build.
bool Renderer::RtMakeIndexBuffer(const std::vector<uint32_t>& indices, RtMesh& mesh) {
  const size_t bytes = indices.size() * sizeof(uint32_t);
  mesh.indices = MakeBuffer(device_, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
  if (!mesh.indices) return false;
  Fill(mesh.indices.Get(), indices.data(), bytes);
  return true;
}

// Top-level structure of the city for this frame: every instance seen in the
// last 900 frames within 4000 units of the first cascade's centre.
bool Renderer::RtBuildCityTlas(const float anchor[3]) {
  rt_city_ready_ = false;
  if ((!rt_device_ && FAILED(device_->QueryInterface(IID_PPV_ARGS(&rt_device_)))) ||
      (!rt_list_ && FAILED(command_list_.As(&rt_list_)))) {
    return false;
  }
  // Static instances (same matrix for 30+ frames) stay 900 frames after they
  // were last drawn, so geometry outside the view keeps casting; moving ones
  // (a new matrix every frame) only 3.
  for (auto it = rt_city_instances_.begin(); it != rt_city_instances_.end();) {
    const RtInstance& in = it->second;
    const uint64_t keep = in.last_swap >= in.first_swap + 30 ? 900 : 3;
    if (swap_number_ > in.last_swap + keep) it = rt_city_instances_.erase(it);
    else ++it;
  }
  for (auto it = rt_meshes_.begin(); it != rt_meshes_.end();) {
    if (swap_number_ > it->second.last_swap + 1800) {
      Retire(std::move(it->second.blas));
      if (it->second.indices) Retire(std::move(it->second.indices));
      it = rt_meshes_.erase(it);
    } else {
      ++it;
    }
  }
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
  descs.reserve(rt_city_instances_.size());
  const float radius2 = 4000.0f * 4000.0f;
  const int32_t excluded = REXCVAR_GET(sr_rt_exclude_sources);
  for (const auto& [key, instance] : rt_city_instances_) {
    if (instance.source & excluded) continue;
    auto mesh = rt_meshes_.find(instance.mesh);
    if (mesh == rt_meshes_.end() || !mesh->second.blas) continue;
    const float* t = instance.transform;
    const float dx = t[3] - anchor[0], dy = t[7] - anchor[1], dz = t[11] - anchor[2];
    if (dx * dx + dy * dy + dz * dz > radius2) continue;
    D3D12_RAYTRACING_INSTANCE_DESC d{};
    std::memcpy(d.Transform, t, sizeof(d.Transform));
    d.InstanceMask = 0xFF;
    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    d.AccelerationStructure = mesh->second.blas->GetGPUVirtualAddress();
    descs.push_back(d);
    if (descs.size() >= 32768) break;
  }
  if (descs.empty()) return false;
  UploadAlloc up;
  const size_t bytes = descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  if (!Upload(bytes, D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT, up)) return false;
  std::memcpy(up.cpu, descs.data(), bytes);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = UINT(descs.size());
  inputs.InstanceDescs = up.gpu;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  rt_device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  if (info.ScratchDataSizeInBytes > rt_build_scratch_size_) {
    if (rt_build_scratch_) Retire(std::move(rt_build_scratch_));
    rt_build_scratch_size_ = std::max<uint64_t>(info.ScratchDataSizeInBytes, 4u << 20);
    rt_build_scratch_ = MakeBuffer(device_, rt_build_scratch_size_, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (!rt_build_scratch_) {
      rt_build_scratch_size_ = 0;
      return false;
    }
  }
  if (info.ResultDataMaxSizeInBytes > rt_city_tlas_size_) {
    if (rt_city_tlas_) Retire(std::move(rt_city_tlas_));
    rt_city_tlas_size_ = info.ResultDataMaxSizeInBytes * 2;
    rt_city_tlas_ = MakeBuffer(device_, rt_city_tlas_size_, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    if (!rt_city_tlas_) {
      rt_city_tlas_size_ = 0;
      return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.RaytracingAccelerationStructure.Location = rt_city_tlas_->GetGPUVirtualAddress();
    device_->CreateShaderResourceView(
        nullptr, &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), kRtTlasSrv));
  }
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = inputs;
  build.DestAccelerationStructureData = rt_city_tlas_->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = rt_build_scratch_->GetGPUVirtualAddress();
  rt_list_->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = nullptr;
  rt_list_->ResourceBarrier(1, &uav);
  rt_city_ready_ = true;
  return true;
}

}  // namespace superman_returns::native
