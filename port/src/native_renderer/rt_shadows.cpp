// Ray-traced sun shadows (docs/cienie-rt-projekt.md): the DXR scene the shadow
// receivers trace with inline RayQuery. Step 2 holds a single test cube placed
// on the sun line through the centre of the game's first cascade (where the
// player is), so its shadow must land next to Superman.

#include "native_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

REXCVAR_DEFINE_DOUBLE(sr_rt_test_cube, 0.0, "Superman Returns Native",
                      "Ray-traced shadows step 2: light-space depth (0..1) of a 6-unit test cube on "
                      "the sun line through the first cascade's centre; 0 = no cube")
    .range(0.0, 1.0);
REXCVAR_DECLARE(bool, sr_rt_alpha_test);
REXCVAR_DEFINE_INT32(sr_rt_ab_max_vertices, 0, "Superman Returns Native",
                     "Debug A/B: with sr_rt_ab_vs, leave out only the character draws of at most this many "
                     "stream-output vertices (0 = all of them)");
REXCVAR_DEFINE_INT32(sr_rt_exclude_sources, 0, "Superman Returns Native",
                     "Debug: leave these capture sources out of the ray-traced scene (bit mask: 1 = indexed "
                     "scene draws, 2 = the shadow caster pass, 4 = instanced draws, 8 = skinned characters)")
    .range(0, 15);
REXCVAR_DEFINE_BOOL(sr_rt_characters, true, "Superman Returns Native",
                    "Ray-traced shadows: skinned characters (Superman, pedestrians) cast shadows (their "
                    "post-skinning positions captured by stream output, one frame behind)");

REXCVAR_DEFINE_BOOL(sr_rt_cape, true, "Superman Returns Native",
                    "Ray-traced scene: Superman's cape (CPU cloth, world-space vertices) is captured with the "
                    "characters, so reflections and ray-traced shadows have it");

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
  if (!RtEnsureScratch(info.ScratchDataSizeInBytes)) return false;
  mesh.blas = buffer_stock_.Take(BufferStock::Kind::kAccelStruct, info.ResultDataMaxSizeInBytes).resource;
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
  mesh.indices = buffer_stock_.Take(BufferStock::Kind::kUpload, bytes).resource;
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
  // (a new matrix every frame) only 3. Expired instances are left out of the
  // TLAS every frame (the loop below). Moving objects' keys are erased through
  // the young queue; walking every instance and mesh, which every frame took 6%
  // of the render worker, happens every 32 frames. A mesh is erased only by
  // that sweep, after its instances.
  const auto expired = [this](const RtInstance& in) {
    const uint64_t keep = in.last_swap >= in.first_swap + 30 ? 900 : 3;
    return swap_number_ > in.last_swap + keep;
  };
  // Instances older than 3 frames leave the young queue: an expired one (a
  // moving object's key of a past frame) is erased now, the rest wait for the
  // sweep.
  while (!rt_young_instances_.empty() && rt_young_instances_.front().second + 3 < swap_number_) {
    auto it = rt_city_instances_.find(rt_young_instances_.front().first);
    if (it != rt_city_instances_.end() && expired(it->second)) rt_city_instances_.erase(it);
    rt_young_instances_.pop_front();
  }
  const bool sweep = (swap_number_ & 31) == 0;
  for (auto it = rt_city_instances_.begin(); sweep && it != rt_city_instances_.end();) {
    if (expired(it->second)) it = rt_city_instances_.erase(it);
    else ++it;
  }
  for (auto it = rt_meshes_.begin(); sweep && it != rt_meshes_.end();) {
    if (swap_number_ > it->second.last_swap + 1800) {
      // Back to the stock: releasing hundreds of buffers at once in the
      // driver stalled the worker for up to 0.3 s.
      buffer_stock_.Give(BufferStock::Kind::kAccelStruct,
                         {std::move(it->second.blas), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE},
                         next_fence_value_);
      if (it->second.indices) {
        buffer_stock_.Give(BufferStock::Kind::kUpload,
                           {std::move(it->second.indices), D3D12_RESOURCE_STATE_GENERIC_READ},
                           next_fence_value_);
      }
      it = rt_meshes_.erase(it);
    } else {
      ++it;
    }
  }
  // Ray-traced reflections: one material entry per TLAS instance (InstanceID =
  // its index): uv pool base (~0 = none), colour texture (SRV | 0x8000 PWL
  // gamma, 0xFFFFFF = none), uv components | sampler << 8, -, uv scale, offset.
  struct Material {
    uint32_t uv_base, texture, comps_sampler, mask;  // mask: tint blend mask texture (0xFFFFFF = none)
    uint32_t mask_sampler, tint_flags, palette;      // palette: index | component << 8 (with flag bit 10)
    float palette_scale;
    float uv[4];
    float tints[4][4];  // R, G, B tint; tint or (palette) building weights
    float a_tint[4];    // (palette) the rest weight's tint
  };
  static_assert(sizeof(Material) == 128);
  const Material kNone{0xFFFFFFFFu, kRtNoAlbedo, 0, kRtNoAlbedo, 0, 0, 0, 0.0f, {1.0f, 1.0f, 0.0f, 0.0f}, {}, {}};
  const float radius2 = 4000.0f * 4000.0f;
  const int32_t excluded = REXCVAR_GET(sr_rt_exclude_sources);
  // Alpha-tested copies (scene draws with a textured alpha test): where one
  // stands, the shadow pass's copy at the same transform is a solid hull.
  const bool ab_swap = RtAbOldSwap(), ab_all = ab_swap && RtAbAll();
  // The city part (thousands of instances, a tenth of the render worker in fast
  // flight) is rebuilt every other frame; the frame between reuses it (an
  // object streamed in joins the rays one frame later). Characters below are
  // added every frame. Sweeps (meshes erased) and A/B frames always rebuild.
  // The city part is built in place in these vectors (copying them was ~6 MB
  // per frame); the characters' entries are appended after it every frame.
  static std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
  static std::vector<Material> materials;
  static size_t city_descs = 0, city_materials = 0;
  static uint64_t city_alpha = 0, city_hulls = 0, city_swap = 0;
  const bool rebuild_city = (swap_number_ & 1) == 0 || sweep || ab_swap || swap_number_ != city_swap + 1;
  uint64_t alpha_instances = 0, hulls_skipped = 0;
  if (!rebuild_city) {
    descs.resize(city_descs);
    materials.resize(city_materials);
    alpha_instances = city_alpha;
    hulls_skipped = city_hulls;
  } else {
  descs.clear();
  materials.clear();
  descs.reserve(rt_city_instances_.size() + 1);
  materials.reserve(rt_city_instances_.size() + 1);
  std::unordered_set<uint64_t> alpha_xforms;
  if ((REXCVAR_GET(sr_rt_alpha_test) && !ab_all)) {
    for (const auto& [key, instance] : rt_city_instances_) {
      const RtMaterial& m = instance.material;
      if (!(instance.source & 2) && m.alpha_ref >= 0.0f && m.albedo != kRtNoAlbedo && m.uv_interp >= 0)
        alpha_xforms.insert(instance.xform);
    }
  }
  for (const auto& [key, instance] : rt_city_instances_) {
    if ((instance.source & 2) && !alpha_xforms.empty() && alpha_xforms.count(instance.xform)) {
      ++hulls_skipped;
      continue;
    }
    if ((instance.source & excluded) || expired(instance)) continue;
    // A/B: the full-scene geometry, or (with a shader list) also the shadow pass's copies.
    if (ab_swap && ((instance.source & 8) || ((instance.source & 2) && !ab_all)) && RtAbHidesVs(instance.vs)) continue;  // A/B: as before the full scene
    auto mesh = rt_meshes_.find(instance.mesh);
    if (mesh == rt_meshes_.end() || !mesh->second.blas) continue;
    const float* t = instance.transform;
    const float dx = t[3] - anchor[0], dy = t[7] - anchor[1], dz = t[11] - anchor[2];
    if (dx * dx + dy * dy + dz * dz > radius2) continue;
    D3D12_RAYTRACING_INSTANCE_DESC d{};
    std::memcpy(d.Transform, t, sizeof(d.Transform));
    // The colour texture while its SRV is still the same texture.
    Material mat = kNone;
    auto live = [this](uint32_t tex, uint32_t gen) {
      return tex != kRtNoAlbedo && SrvGeneration(tex & 0x7FFFu) == gen;
    };
    auto usable = [&](const RtMaterial& m) {
      return live(m.albedo, m.albedo_gen) || live(m.mask, m.mask_gen);
    };
    const RtMaterial* src = usable(instance.material) ? &instance.material
                            : usable(mesh->second.material) ? &mesh->second.material : nullptr;
    if (src) {
      mat.texture = live(src->albedo, src->albedo_gen) ? src->albedo : kRtNoAlbedo;
      mat.comps_sampler = src->uv_comps | (src->sampler << 8);
      if (live(src->mask, src->mask_gen)) {
        mat.mask = src->mask;
        mat.mask_sampler = src->mask_sampler;
        mat.tint_flags = src->tint_flags;
        if (mat.texture == kRtNoAlbedo) mat.tint_flags &= ~(1u << 8);  // colour texture retired
        mat.tint_flags |= 1u << 9;  // tint blend
        std::memcpy(mat.tints, src->tints, sizeof(mat.tints));
        if ((src->tint_flags >> 10) & 1) {
          if (src->palette_interp == mesh->second.palette_interp && src->palette >= 0) {
            std::memcpy(mat.a_tint, src->a_tint, sizeof(mat.a_tint));
            mat.palette = uint32_t(src->palette) | (src->palette_comp << 8);
            mat.palette_scale = src->palette_scale;
          } else {
            mat.tint_flags &= ~(1u << 10);
          }
        }
      }
      if (src->uv_interp >= 0 && src->uv_interp == mesh->second.uv_interp) {
        mat.uv_base = mesh->second.uv_base;
        std::memcpy(mat.uv, src->uv, sizeof(mat.uv));
      }
    }
    d.InstanceID = UINT(materials.size());
    // Mask 1: has a material (reflection rays try these first), 2: none.
    d.InstanceMask = mat.texture != kRtNoAlbedo || ((mat.tint_flags >> 9) & 1) ? 0x01 : 0x02;
    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    // Alpha test (flag bit 12, threshold in byte 124): the rays look at the
    // colour texture's alpha at the hit (non-opaque candidates).
    if (src && src->alpha_ref >= 0.0f && mat.texture != kRtNoAlbedo && mat.uv_base != 0xFFFFFFFFu &&
        !((mat.tint_flags >> 9) & 1) && (REXCVAR_GET(sr_rt_alpha_test) && !ab_all)) {
      mat.tint_flags |= 1u << 12;
      mat.a_tint[3] = src->alpha_ref;
      d.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
      ++alpha_instances;
    }
    materials.push_back(mat);
    d.AccelerationStructure = mesh->second.blas->GetGPUVirtualAddress();
    descs.push_back(d);
    if (descs.size() >= 31744) break;  // room for the character draws' materials
  }
  city_descs = descs.size();
  city_materials = materials.size();
  city_alpha = alpha_instances;
  city_hulls = hulls_skipped;
  }
  if (rebuild_city) city_swap = swap_number_;
  if (rt_skin_ready_ && !(excluded & 8)) {
    D3D12_RAYTRACING_INSTANCE_DESC d{};
    std::memcpy(d.Transform, rt_skin_transform_, sizeof(d.Transform));
    // One material per character draw (the BLAS geometries, GeometryIndex);
    // their texture coordinates are in the character buffer (tint flag bit 11).
    // Mask 1: real geometry, nothing duplicates it.
    d.InstanceID = UINT(materials.size());
    d.InstanceMask = 0x01;
    for (const RtSkinDraw& draw : rt_skin_draws_) {
      Material mat = kNone;
      const RtMaterial& m = draw.material;
      if (draw.has_material && m.albedo != kRtNoAlbedo && SrvGeneration(m.albedo & 0x7FFFu) == m.albedo_gen) {
        mat.texture = m.albedo;
        mat.comps_sampler = m.uv_comps | (m.sampler << 8);
        mat.tint_flags = 1u << 11;
        if (m.uv_interp >= 0) {
          mat.uv_base = uint32_t(draw.offset / 32);
          std::memcpy(mat.uv, m.uv, sizeof(mat.uv));
        }
      }
      materials.push_back(mat);
    }
    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    d.AccelerationStructure = rt_skin_blas_->GetGPUVirtualAddress();
    descs.push_back(d);
  }
  if (descs.empty()) return false;
  // The whole table every frame (uploading only the characters' entries between
  // rebuilds gave wrong colours in game, 2026-11-08).
  RtUploadMaterials(materials.data(), materials.size() * sizeof(Material));
  RtUploadPalettes();
  static uint64_t last_material_log = 0;
  if (swap_number_ >= last_material_log + 600) {
    last_material_log = swap_number_;
    size_t textured = 0, with_uv = 0, own = 0, stale = 0, tinted = 0;
    for (const Material& m : materials) {
      textured += m.texture != kRtNoAlbedo;
      with_uv += m.uv_base != 0xFFFFFFFFu;
      tinted += (m.tint_flags >> 9) & 1;
    }
    for (const auto& [key, instance] : rt_city_instances_) {
      own += instance.material.albedo != kRtNoAlbedo;
      stale += instance.material.albedo != kRtNoAlbedo &&
               SrvGeneration(instance.material.albedo & 0x7FFFu) != instance.material.albedo_gen;
    }
    REXLOG_INFO("native: rt materials: {} instances, {} with a colour texture, {} tint blends, {} with texture "
                "coordinates; {} copies drawn textured, {} of them with a retired texture; {} alpha-tested, {} "
                "shadow-pass hulls left out",
                materials.size(), textured, tinted, with_uv, own, stale, alpha_instances, hulls_skipped);
  }
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
  if (!RtEnsureScratch(info.ScratchDataSizeInBytes)) return false;
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

// The frame's material table (kRtMaterialSrv): copied from the upload ring
// into a default-heap buffer the reflection rays read by InstanceID.
bool Renderer::RtUploadMaterials(const void* data, size_t bytes, size_t offset) {
  if (offset >= kRtMaterialBytes) return false;
  bytes = std::min(bytes, kRtMaterialBytes - offset);
  if (!rt_material_buffer_) {
    rt_material_buffer_ = MakeBuffer(device_, kRtMaterialBytes + kRtPaletteBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false);
    if (!rt_material_buffer_) return false;
    rt_material_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = UINT((kRtMaterialBytes + kRtPaletteBytes) / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(
        rt_material_buffer_.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), kRtMaterialSrv));
  }
  UploadAlloc up;
  if (!bytes) return true;
  if (!Upload(bytes, 256, up)) return false;
  std::memcpy(up.cpu, data, bytes);
  Transition(rt_material_buffer_.Get(), rt_material_state_, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list_->CopyBufferRegion(rt_material_buffer_.Get(), offset, up.resource, up.offset, bytes);
  Transition(rt_material_buffer_.Get(), rt_material_state_,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return true;
}

// Scratch memory shared by every acceleration structure build of the frame.
bool Renderer::RtEnsureScratch(uint64_t size) {
  if (size <= rt_build_scratch_size_) return true;
  if (rt_build_scratch_) Retire(std::move(rt_build_scratch_));
  rt_build_scratch_size_ = std::max<uint64_t>(size, 4u << 20);
  rt_build_scratch_ = MakeBuffer(device_, rt_build_scratch_size_, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  if (!rt_build_scratch_) rt_build_scratch_size_ = 0;
  return rt_build_scratch_ != nullptr;
}

namespace {
constexpr uint64_t kSoBytes = 64ull << 20;          // character stream output of one frame (32 B a vertex)
constexpr uint32_t kSoSlots = 1024, kSoSlotBytes = 16;  // filled-size counter per captured draw
}  // namespace

// Skinned characters: the draw just recorded is repeated through a
// stream-output pipeline (same vertex shader and input layout, nothing
// rasterised) that keeps SRLIGHT0.xyz, the light-space position after
// skinning, as a triangle soup in rt_so_buffer_. Only draws using the frame's
// light rows: one inverse transform then places every soup in the world.
void Renderer::RtCaptureSkinned(uint8_t* base, uint32_t u, uint32_t v, uint32_t z, uint32_t start_index,
                                uint32_t index_count, int32_t base_vertex, uint32_t so_vertices) {
  if (!REXCVAR_GET(sr_rt_characters) || SkipGpuDraw() || rt_frame_swap_ != swap_number_ ||
      !rt_so_target_.pso) {
    return;
  }
  const float* c = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase);
  for (int k = 0; k < 4; ++k) {
    if (std::abs(c[u * 4 + k] - rt_frame_rows_[k]) > 1e-4f ||
        std::abs(c[v * 4 + k] - rt_frame_rows_[4 + k]) > 1e-4f ||
        std::abs(c[z * 4 + k] - rt_frame_rows_[8 + k]) > 1e-4f) {
      ++rt_stats_.skip_skinned;
      return;
    }
  }
  const uint32_t vertices = so_vertices;
  if (!vertices || rt_so_cursor_ + uint64_t(vertices) * 32 > kSoBytes) {
    ++rt_stats_.skip_skinned;
    return;
  }
  if (!rt_so_buffers_[0]) {
    for (uint32_t i = 0; i < 2; ++i) {
      rt_so_buffers_[i] = MakeBuffer(device_, kSoBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT,
                                     false);
      if (!rt_so_buffers_[i]) {
        rt_so_buffers_[0].Reset();
        rt_so_buffers_[1].Reset();
        return;
      }
      rt_so_states_[i] = D3D12_RESOURCE_STATE_STREAM_OUT;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = DXGI_FORMAT_R32_TYPELESS;
      srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Buffer.NumElements = UINT(kSoBytes / 4);
      srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
      device_->CreateShaderResourceView(
          rt_so_buffers_[i].Get(), &srv,
          provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), kRtSkinSrv0 - i));
    }
  }
  // Each vertex: the pixel shader's UV interpolant (SV_Position as filler
  // without one), then SRLIGHT0, the light-space position after skinning.
  const int32_t uv_interp = RtDrawUvInterp();
  ID3D12PipelineState* pso = uv_interp >= 0 ? RtSoPipeline(base, "TEXCOORD", uint32_t(uv_interp), 4, "SRLIGHT", 0, 4)
                                            : RtSoPipeline(base, "SV_Position", 0, 4, "SRLIGHT", 0, 4);
  if (!pso || !RtSoBeginDraw()) return;
  ID3D12Resource* buffer = rt_so_buffers_[rt_so_write_].Get();
  Transition(buffer, rt_so_states_[rt_so_write_], D3D12_RESOURCE_STATE_STREAM_OUT);
  if (rt_so_pending_.empty()) std::memcpy(rt_skin_transform_, rt_frame_inv_, sizeof(rt_skin_transform_));
  RtSoDraw(pso, buffer->GetGPUVirtualAddress() + rt_so_cursor_, uint64_t(vertices) * 32, true, index_count,
           start_index, base_vertex);
  RtSkinDraw draw{rt_so_cursor_, vertices, {}, false};
  draw.has_material = RtDrawMaterial(uv_interp, draw.material);
  static std::unordered_set<uint64_t> logged_ps;
  if (logged_ps.size() < 16 && logged_ps.insert(current_ps_hash_).second)
    REXLOG_INFO("native: rt character ps {:016X}: uv interpolant {}, material {} (texture {:X} uv {})",
                current_ps_hash_, uv_interp, draw.has_material, draw.material.albedo, draw.material.uv_interp);
  draw.vs = current_vs_hash_;
  rt_so_pending_.push_back(draw);
  rt_so_cursor_ += uint64_t(vertices) * 32;
  ++rt_stats_.skinned_draws;
}

// Superman's cape (vertex shader 4536A1CD): cloth simulated on the CPU, world-
// space vertices in a new buffer every frame, drawn with the camera only (no
// world matrix, no light rows), so neither the city nor the character capture
// took it and reflections showed Superman without it. Its vertex shader
// writes TEXCOORD0 = UV and TEXCOORD1 = camera - world; stream output keeps
// both in the character soup, and the build maps the view vector into light
// space (light = rows * (camera - v)) so it shares the characters' BLAS,
// instance and materials.
void Renderer::RtCaptureCape(uint8_t* base, bool indexed, uint32_t start, uint32_t count, int32_t base_vertex,
                             uint32_t so_vertices) {
  if (!REXCVAR_GET(sr_rt_characters) || !REXCVAR_GET(sr_rt_cape) || SkipGpuDraw() ||
      rt_frame_swap_ != swap_number_ || !rt_so_target_.pso || !so_vertices || !rt_so_buffers_[0]) {
    return;
  }
  if (rt_so_cursor_ + uint64_t(so_vertices) * 32 > kSoBytes) return;
  const float* c = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase);
  const float cam[3] = {c[0 * 4 + 3], c[1 * 4 + 3], c[2 * 4 + 3]};  // gViewInverse (register 0) column w
  const int32_t uv_interp = RtDrawUvInterp();
  ID3D12PipelineState* pso = RtSoPipeline(base, "TEXCOORD", uint32_t(uv_interp >= 0 ? uv_interp : 0), 4,
                                          "TEXCOORD", 1, 4);
  if (!pso || !RtSoBeginDraw()) return;
  ID3D12Resource* buffer = rt_so_buffers_[rt_so_write_].Get();
  Transition(buffer, rt_so_states_[rt_so_write_], D3D12_RESOURCE_STATE_STREAM_OUT);
  if (rt_so_pending_.empty()) std::memcpy(rt_skin_transform_, rt_frame_inv_, sizeof(rt_skin_transform_));
  RtSoDraw(pso, buffer->GetGPUVirtualAddress() + rt_so_cursor_, uint64_t(so_vertices) * 32, indexed, count, start,
           base_vertex);
  RtSkinDraw draw{rt_so_cursor_, so_vertices, {}, false};
  draw.has_material = RtDrawMaterial(uv_interp, draw.material);
  draw.cape = true;
  for (int r = 0; r < 3; ++r) {
    const float* row = rt_frame_rows_ + 4 * r;
    draw.to_light[4 * r + 0] = -row[0];
    draw.to_light[4 * r + 1] = -row[1];
    draw.to_light[4 * r + 2] = -row[2];
    draw.to_light[4 * r + 3] = row[0] * cam[0] + row[1] * cam[1] + row[2] * cam[2] + row[3];
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("native: rt cape: {} vertices, ps {:016X}, uv interpolant {}, material {} (texture {:X}), camera "
                "{:.1f} {:.1f} {:.1f}",
                so_vertices, current_ps_hash_, uv_interp, draw.has_material, draw.material.albedo, cam[0], cam[1],
                cam[2]);
  }
  draw.vs = current_vs_hash_;
  rt_so_pending_.push_back(draw);
  rt_so_cursor_ += uint64_t(so_vertices) * 32;
  ++rt_stats_.cape_draws;
}

// The stream-output pipeline repeating the current draw: its vertex shader,
// input layout and target formats, nothing rasterised, one output element
// (semantic + index, the first `components` components).
// semantic2: an optional second element after the first (stride = both).
ID3D12PipelineState* Renderer::RtSoPipeline(uint8_t* base, const char* semantic, uint32_t index,
                                            uint32_t components, const char* semantic2, uint32_t index2,
                                            uint32_t components2) {
  const uint32_t decl = rt_so_target_.decl;
  uint64_t key = current_vs_hash_;
  auto mix = [&key](uint64_t x) { key ^= x + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
  mix(rt_so_target_.decl_hash);
  mix(rt_so_target_.rt_count);
  for (DXGI_FORMAT f : rt_so_target_.rtv) mix(uint64_t(f));
  mix(uint64_t(rt_so_target_.dsv));
  mix(bound_samples_);
  mix(uint64_t(rt_so_target_.strip_cut));
  for (const char* c = semantic; *c; ++c) mix(uint64_t(uint8_t(*c)));
  mix((uint64_t(index) << 8) | components);
  for (const char* c = semantic2 ? semantic2 : ""; *c; ++c) mix(uint64_t(uint8_t(*c)));
  mix((uint64_t(index2) << 8) | components2);
  auto [pit, fresh] = rt_so_pipelines_.try_emplace(key);
  if (fresh) {
    const std::vector<uint8_t>* vs = LoadShader(current_vs_hash_, true);
    if (!vs) {
      rt_so_pipelines_.erase(pit);  // still translating: ask again next time
      return nullptr;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    BuildInputLayout(base, decl, *vs, elements, nullptr);
    const D3D12_SO_DECLARATION_ENTRY so[2] = {
        {0, semantic, index, 0, uint8_t(components), 0},
        {0, semantic2, index2, 0, uint8_t(components2), 0}};
    const UINT stride = (components + (semantic2 ? components2 : 0)) * 4;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature_.Get();
    desc.VS = {vs->data(), vs->size()};
    desc.InputLayout = {elements.data(), uint32_t(elements.size())};
    desc.StreamOutput = {so, semantic2 ? 2u : 1u, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
    for (auto& rt : desc.BlendState.RenderTarget) {
      rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
      rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
      rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      rt.LogicOp = D3D12_LOGIC_OP_NOOP;
      rt.RenderTargetWriteMask = 0;
    }
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DSVFormat = rt_so_target_.dsv;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.IBStripCutValue = rt_so_target_.strip_cut;
    desc.NumRenderTargets = rt_so_target_.rt_count;
    for (uint32_t i = 0; i < rt_so_target_.rt_count && i < 4; ++i) {
      desc.RTVFormats[i] = rt_so_target_.rtv[i] != DXGI_FORMAT_UNKNOWN ? rt_so_target_.rtv[i] : rt_so_target_.rtv[0];
    }
    if (rt_so_target_.rt_count == 5) desc.RTVFormats[4] = kDlssMotionFormat;  // DLSS objects' motion
    desc.SampleDesc.Count = bound_samples_;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pit->second));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (FAILED(hr)) {
      REXLOG_ERROR("native: rt stream-output pipeline for vs {:016X} {}{} failed ({:08X})", current_vs_hash_,
                   semantic, index, uint32_t(hr));
    } else {
      REXLOG_INFO("native: rt stream-output pipeline for vs {:016X} {}{} ({} pipelines) in {:.1f} ms",
                  current_vs_hash_, semantic, index, rt_so_pipelines_.size(), ms);
    }
  }
  return pit->second.Get();
}

// A filled-size counter for the next stream-output draw of this frame (the
// counters start at 0 every frame: stream output appends after them).
bool Renderer::RtSoBeginDraw() {
  if (!rt_so_counters_) {
    rt_so_counters_ = MakeBuffer(device_, uint64_t(kSoSlots) * kSoSlotBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_STREAM_OUT, false);
    if (!rt_so_counters_) return false;
  }
  if (rt_so_swap_ != swap_number_) {
    UploadAlloc zero;
    const size_t bytes = size_t(kSoSlots) * kSoSlotBytes;
    if (!Upload(bytes, 256, zero)) return false;
    std::memset(zero.cpu, 0, bytes);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_STREAM_OUT;
    Transition(rt_so_counters_.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyBufferRegion(rt_so_counters_.Get(), 0, zero.resource, zero.offset, bytes);
    Transition(rt_so_counters_.Get(), state, D3D12_RESOURCE_STATE_STREAM_OUT);
    rt_so_swap_ = swap_number_;
    rt_so_draws_ = 0;
  }
  return rt_so_draws_ < kSoSlots;
}

// Repeats the current draw (indexed: count indices from start; else count
// vertices from start) through a stream-output pipeline into dst (a buffer in
// the STREAM_OUT state), then restores the draw's pipeline.
void Renderer::RtSoDraw(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS dst, uint64_t bytes, bool indexed,
                        uint32_t count, uint32_t start, int32_t base_vertex) {
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{
      dst, bytes, rt_so_counters_->GetGPUVirtualAddress() + uint64_t(rt_so_draws_) * kSoSlotBytes};
  command_list_->SetPipelineState(pso);
  command_list_->SOSetTargets(0, 1, &view);
  if (indexed) command_list_->DrawIndexedInstanced(count, 1, start, base_vertex, 0);
  else command_list_->DrawInstanced(count, 1, start, 0);
  command_list_->SOSetTargets(0, 1, nullptr);
  command_list_->SetPipelineState(rt_so_target_.pso);
  ++rt_so_draws_;
}

// Ray-traced reflections: the texture coordinates of a new city mesh. Its
// draw is repeated once through a stream-output pipeline keeping the
// interpolant TEXCOORD<interp> its pixel shader derives the colour texture's
// UV from (all 4 components; the table names which two and their scale and
// offset), as a triangle soup in BLAS primitive order: vertex 3p + j of the
// pool is corner j of primitive p. Once per mesh, at most 64 meshes a frame.
void Renderer::RtCaptureUv(RtMesh& mesh, uint8_t* base, int32_t interp, int32_t palette_interp, bool indexed,
                           uint32_t count, uint32_t start, int32_t base_vertex, uint32_t so_count) {
  if (interp < 0 || mesh.uv_tried || !rt_so_target_.pso || SkipGpuDraw()) return;
  if (rt_uv_swap_ != swap_number_) {
    rt_uv_swap_ = swap_number_;
    rt_uv_this_frame_ = 0;
  }
  if (rt_uv_this_frame_ >= 64) return;
  ++rt_uv_this_frame_;
  mesh.uv_tried = true;
  const uint32_t vertices = so_count ? so_count : count - count % 3;
  constexpr uint64_t kUvVertices = (192ull << 20) / 32;  // 32 bytes: UV interpolant, palette interpolant
  if (!vertices || rt_uv_cursor_ + vertices > kUvVertices) return;
  if (!rt_uv_pool_) {
    rt_uv_pool_ = MakeBuffer(device_, kUvVertices * 32, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_STREAM_OUT, false);
    if (!rt_uv_pool_) return;
    rt_uv_state_ = D3D12_RESOURCE_STATE_STREAM_OUT;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = UINT(kUvVertices * 8);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(
        rt_uv_pool_.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), kRtUvSrv));
  }
  ID3D12PipelineState* pso =
      RtSoPipeline(base, "TEXCOORD", uint32_t(interp), 4, palette_interp >= 0 ? "TEXCOORD" : "SV_Position",
                   uint32_t(std::max(palette_interp, 0)), 4);
  if (!pso || !RtSoBeginDraw()) return;
  Transition(rt_uv_pool_.Get(), rt_uv_state_, D3D12_RESOURCE_STATE_STREAM_OUT);
  RtSoDraw(pso, rt_uv_pool_->GetGPUVirtualAddress() + rt_uv_cursor_ * 32, uint64_t(vertices) * 32, indexed,
           count, start, base_vertex);
  Transition(rt_uv_pool_.Get(), rt_uv_state_,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  mesh.uv_base = uint32_t(rt_uv_cursor_);
  mesh.uv_interp = interp;
  mesh.palette_interp = palette_interp;
  rt_uv_cursor_ += vertices;
  ++rt_stats_.uv_meshes;
}

// One BLAS for every character captured in the previous frame (their
// stream-output soups, light space; the TLAS instances it with the inverse
// light rows of that frame). Rebuilt in place: the TLAS that used it last was
// consumed earlier on this queue.
bool Renderer::RtBuildSkinnedBlas() {
  rt_skin_ready_ = false;
  if (rt_so_pending_.empty() || !rt_so_buffers_[0]) return false;
  ID3D12Resource* buffer = rt_so_buffers_[rt_so_write_].Get();
  if ((!rt_device_ && FAILED(device_->QueryInterface(IID_PPV_ARGS(&rt_device_)))) ||
      (!rt_list_ && FAILED(command_list_.As(&rt_list_)))) {
    return false;
  }
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry(rt_so_pending_.size());
  uint64_t triangles = 0;
  // The cape's view-vector soups into light space (RtCaptureCape).
  size_t capes = 0;
  for (const RtSkinDraw& d : rt_so_pending_) capes += d.cape;
  UploadAlloc xforms{};
  if (capes && !Upload(capes * 48, 16, xforms)) return false;
  size_t cape_slot = 0;
  for (size_t i = 0; i < geometry.size(); ++i) {
    if (rt_so_pending_[i].cape) {
      std::memcpy(static_cast<uint8_t*>(xforms.cpu) + cape_slot * 48, rt_so_pending_[i].to_light, 48);
      geometry[i].Triangles.Transform3x4 = xforms.gpu + cape_slot * 48;
      ++cape_slot;
    }
    D3D12_RAYTRACING_GEOMETRY_DESC& g = geometry[i];
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.Triangles.VertexBuffer.StartAddress = buffer->GetGPUVirtualAddress() + rt_so_pending_[i].offset + 16;
    g.Triangles.VertexBuffer.StrideInBytes = 32;
    g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexCount = rt_so_pending_[i].vertices;
    // A/B by shader (sr_rt_ab_vs): the draw stays (its material slot) but empty.
    if (!RtAbAll() && RtAbHidesVs(rt_so_pending_[i].vs) &&
        (REXCVAR_GET(sr_rt_ab_max_vertices) <= 0 ||
         rt_so_pending_[i].vertices <= uint32_t(REXCVAR_GET(sr_rt_ab_max_vertices))))
      g.Triangles.VertexCount = 0;
    triangles += rt_so_pending_[i].vertices / 3;
  }
  const size_t draws = rt_so_pending_.size();
  rt_skin_draws_.swap(rt_so_pending_);
  rt_so_pending_.clear();
  rt_so_cursor_ = 0;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = UINT(geometry.size());
  inputs.pGeometryDescs = geometry.data();
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  rt_device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  if (!info.ResultDataMaxSizeInBytes || !RtEnsureScratch(info.ScratchDataSizeInBytes)) return false;
  if (info.ResultDataMaxSizeInBytes > rt_skin_blas_size_) {
    if (rt_skin_blas_) Retire(std::move(rt_skin_blas_));
    rt_skin_blas_size_ = info.ResultDataMaxSizeInBytes * 3 / 2;
    rt_skin_blas_ = MakeBuffer(device_, rt_skin_blas_size_, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    if (!rt_skin_blas_) {
      rt_skin_blas_size_ = 0;
      return false;
    }
  }
  // Read by the build and, this frame, by the reflection hits (texture coordinates).
  Transition(buffer, rt_so_states_[rt_so_write_],
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = inputs;
  build.DestAccelerationStructureData = rt_skin_blas_->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = rt_build_scratch_->GetGPUVirtualAddress();
  rt_list_->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = nullptr;  // scratch reuse and the TLAS build
  rt_list_->ResourceBarrier(1, &uav);
  rt_skin_read_ = rt_so_write_;
  rt_so_write_ ^= 1u;
  rt_skin_ready_ = true;
  static uint64_t last_log = 0;
  if (swap_number_ >= last_log + 600) {
    last_log = swap_number_;
    REXLOG_INFO("native: rt characters: {} draws ({} cape), {} triangles in one BLAS ({} KB); captured {} "
                "(capes {}) skipped {}",
                draws, capes, triangles, info.ResultDataMaxSizeInBytes >> 10, rt_stats_.skinned_draws,
                rt_stats_.cape_draws, rt_stats_.skip_skinned);
  }
  return true;
}

}  // namespace superman_returns::native
