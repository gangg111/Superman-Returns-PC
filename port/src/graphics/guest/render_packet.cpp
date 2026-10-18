#include "render_packet.h"
#include "state_source.h"
#include "../../native_renderer/game_profile.h"
#include "../../native_renderer/pm4_mirror.h"
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
namespace superman_returns::graphics::guest {
namespace {
bool DecodeCapturedPacket(const WorkBatch&, const WorkCmd&,
                          const native::Pm4Mirror&, const CapturedMemory&,
                          RenderPacket&, std::string&,DrawPacketOwner={});
}
bool ReplayCapturedRenderPacket(const WorkBatch& batch,const WorkCmd& cmd,native::Pm4Mirror& mirror,RenderPacket& packet,std::string& error,DrawPacketOwner storage,std::shared_ptr<const WorkBatch> capture_owner,ReplayProbe* probe) {
  error.clear();
  if(!cmd.pm4_capture_ok) {error="PM4 dependency capture failed";return false;}
  if(uint64_t(cmd.ring_offset)+cmd.ring_bytes>batch.bytes.size()) {error="Captured primary PM4 range exceeds batch";return false;}
  if(capture_owner && capture_owner.get()!=&batch) {error="Capture owner does not match replay batch";return false;}
  const auto now=[] {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
  auto started=probe?now():0;
  CapturedMemory memory;
  if(!(capture_owner?CapturedMemory::CaptureOwned(std::move(capture_owner),cmd,memory,error):
                    CapturedMemory::Capture(batch,cmd,memory,error))) return false;
  if(probe) {auto end=now();probe->capture_ns+=end-started;started=end;}
  if(cmd.ring_bytes) {
    auto alu=mirror.unreadable_alu_loads,indirect=mirror.unreadable_indirect_buffers;std::string missing;
    mirror.ScanCopyUsing(batch.bytes.data()+cmd.ring_offset,cmd.ring_bytes,[&](uint32_t address,uint32_t length)->std::span<const uint8_t> {
      try {return memory.Read(address,length);} catch(const std::out_of_range& ex) {missing=ex.what();return {};}
    });
    if(!missing.empty() || mirror.unreadable_alu_loads!=alu || mirror.unreadable_indirect_buffers!=indirect) {error=missing.empty()?"Captured PM4 dependency could not be read":missing;return false;}
  }
  if(probe) {auto end=now();probe->pm4_ns+=end-started;started=end;}
  const bool ok=DecodeCapturedPacket(batch,cmd,mirror,memory,packet,error,std::move(storage));
  if(probe) probe->decode_ns+=now()-started;
  return ok;
}
namespace {
uint32_t Address(uint32_t base, uint64_t offset) {
  if (uint64_t(base) + offset > UINT32_MAX)
    throw std::out_of_range("Guest address overflow");
  return uint32_t(base + offset);
}
uint32_t U32(const CapturedMemory &m, uint32_t a) {
  auto b = m.Read(a, 4);
  return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 |
         b[3];
}
uint32_t CapturedWord(std::span<const uint8_t> bytes,uint32_t offset) {
  return uint32_t(bytes[offset])<<24 | uint32_t(bytes[offset+1])<<16 |
         uint32_t(bytes[offset+2])<<8 | bytes[offset+3];
}
uint16_t U16(const CapturedMemory &m, uint32_t a) {
  auto b = m.Read(a, 2);
  return uint16_t(b[0]) << 8 | b[1];
}
std::vector<Rect> Rects(const CapturedMemory &m, uint32_t address,
                        uint32_t count) {
  std::vector<Rect> out;
  if (!address)
    return out;
  for (uint32_t i = 0; i < std::min(count, 64u); ++i) {
    uint32_t a = Address(address, uint64_t(i) * 16);
    out.push_back({int32_t(U32(m, a)), int32_t(U32(m, Address(a, 4))),
                   int32_t(U32(m, Address(a, 8))),
                   int32_t(U32(m, Address(a, 12)))});
  }
  return out;
}
std::array<float, 4> Color(const CapturedMemory &m, uint32_t a) {
  std::array<float, 4> out{};
  if (a)
    for (uint32_t i = 0; i < 4; ++i)
      out[i] = std::bit_cast<float>(U32(m, Address(a, 4 * i)));
  return out;
}
BufferUpdate Buffer(const CapturedMemory &m, const BufferPlan &plan) {
  BufferUpdate out;
  out.plan = plan;
  if (plan.action) {
    if (plan.action > 2 || plan.begin > plan.end || plan.end > plan.size)
      throw std::out_of_range("Invalid buffer update range");
    auto data =
        m.Read(Address(0xa0000000u, uint64_t(plan.address) + plan.begin),
               plan.end - plan.begin);
    out.bytes.assign(data.begin(), data.end());
  }
  return out;
}
SurfaceDesc Surface(const CapturedMemory &m, uint32_t object, bool depth) {
  if (!object)
    return {};
  return {object, depth,
          DecodeSurfaceGeometry(U32(m, Address(object, 0x18)),
                                U32(m, Address(object, 0x1c)),
                                U32(m, Address(object, 0x24)), depth)};
}
void DeviceState(DrawPacket &d, uint32_t device,
                 const native::Pm4Mirror &mirror) {
  const auto &layout = native::profile::kDevice;
  for (const auto &range : native::profile::kRegisterShadow) {
    const auto address=Address(device,range.offset);
    const auto bytes=d.memory.ReadUnambiguous(address,range.count*4);
    for (uint32_t i = 0; i < range.count; ++i) {
      uint32_t reg = range.first + i;
      if (reg >= 0x2000 && reg < 0x2400)
        d.registers[reg - 0x2000] =
            bytes.empty()?U32(d.memory, Address(address,uint64_t(i)*4)):CapturedWord(bytes,i*4);
    }
  }
  uint32_t a = Address(device, layout.viewport);
  d.viewport = NormalizeViewport(
      {float(U32(d.memory, a)), float(U32(d.memory, Address(a, 4))),
       float(U32(d.memory, Address(a, 8))),
       float(U32(d.memory, Address(a, 12))),
       std::bit_cast<float>(U32(d.memory, Address(a, 16))),
       std::bit_cast<float>(U32(d.memory, Address(a, 20)))},
      {1280, 720}, std::nullopt, 1);
  for (uint32_t i = 0; i < 4; ++i)
    d.colors[i] = Surface(
        d.memory, U32(d.memory, Address(device, layout.render_targets + i * 4)),
        false);
  d.depth = Surface(d.memory,
                    U32(d.memory, Address(device, layout.depth_stencil)), true);
  d.shader_a = U32(d.memory, Address(device, layout.shader_a));
  d.shader_b = U32(d.memory, Address(device, layout.shader_b));
  const auto fetch_address=Address(device,layout.fetch_constants);
  const auto fetch_bytes=d.memory.ReadUnambiguous(fetch_address,32*24);
  for (uint32_t slot = 0; slot < 32; ++slot)
    for (uint32_t i = 0; i < 6; ++i) {
      uint32_t reg = native::Pm4Mirror::kFetchConstantBase + slot * 6 + i;
      d.texture_fetch[slot][i] =
          mirror.written(reg)
              ? mirror.reg(reg)
              : (fetch_bytes.empty()?U32(d.memory, Address(fetch_address,slot*24+i*4)):
                 CapturedWord(fetch_bytes,slot*24+i*4));
    }
  d.has_device_state = true;
}
} // namespace
void FinalizeDrawConstants(DrawPacket &d) {
  uint32_t mode=d.registers[0x205];float scale=0,offset=0;
  bool polygonal=d.primitive==Primitive::kTriangles || d.primitive==Primitive::kTriangleStrip || d.primitive==Primitive::kQuads || d.primitive==Primitive::kRectangles;
  auto read_offset=[&](uint32_t first) {scale=std::bit_cast<float>(d.polygon_offset_registers[first]);offset=std::bit_cast<float>(d.polygon_offset_registers[first+1]);};
  if(polygonal) {
    if((mode&(1<<11)) && !(mode&1)) read_offset(0);
    if((mode&(1<<12)) && !(mode&2) && !scale && !offset) read_offset(2);
  } else if(mode&(1<<13)) read_offset(0);
  if(!std::isfinite(scale)) scale=0;if(!std::isfinite(offset)) offset=0;
  // Same D24 conversion used by the reference GetD3D10IntegerPolygonOffset.
  auto magnitude=std::min(double(std::ceil(std::abs(offset)*16777215.0f)),double(INT32_MAX));
  d.depth_bias=offset<0?-int32_t(magnitude):int32_t(magnitude);d.slope_bias=scale/16;
  auto put = [&](uint32_t word, uint32_t v) {
    std::memcpy(d.constants.shared.data() + word * 4, &v, 4);
  };
  auto putf = [&](uint32_t word, float f) {
    put(word, std::bit_cast<uint32_t>(f));
  };
  bool screen = d.has_device_state && !(d.registers[0x206] & 1u);
  Extent target{1280, 720};
  if (d.colors[0].id)
    target = {float(d.colors[0].geometry.width),
              float(d.colors[0].geometry.height)};
  else if (d.depth.id)
    target = {float(d.depth.geometry.width), float(d.depth.geometry.height)};
  if (d.has_device_state)
    d.viewport =
        NormalizeViewport(d.viewport, {1280, 720},
                          screen ? std::optional(target) : std::nullopt, 1);
  auto size = screen ? target : Extent{d.viewport.width, d.viewport.height};
  if (size.width <= 0 || size.height <= 0)
    size = {1280, 720};
  bool half = d.has_device_state && !(d.registers[0x302] & 1u);
  putf(73, half ? 1.0f / size.width : 0);
  putf(74, half ? -1.0f / size.height : 0);
  uint32_t spec = 0;
  for (const auto &a : d.attributes) {
    if (a.usage != 3 && a.usage != 6 && a.usage != 7)
      continue;
    if (a.type == 0x2a2187)
      spec |= 1u << 6;
    if (a.type == 0x2a2190 || a.type == 0x2a2390)
      spec |= 1u;
  }
  uint32_t control = d.registers[0x202], func = control & 7;
  float alpha = 0;
  if ((control & 8) && func != 7) {
    spec |= 2;
    alpha = func == 0 ? 2.0f : std::bit_cast<float>(d.registers[0x10e]);
  }
  putf(75, alpha);
  put(112, spec);
  putf(113, 1);
  putf(114, 1);
  putf(115, 0);
  float xform[4] = {1, 1, 0, 0};
  if (screen) {
    uint32_t win = d.registers[0x80];
    float wx = float(int32_t(win << 17) >> 17),
          wy = float(int32_t((win >> 16) << 17) >> 17);
    xform[0] = 2 / target.width;
    xform[1] = -2 / target.height;
    xform[2] = -1 + wx * xform[0];
    xform[3] = 1 + wy * xform[1];
  }
  std::memcpy(d.constants.shared.data() + 108 * 4, xform, sizeof(xform));
}
bool DecodeRenderPacket(const WorkBatch &batch, const WorkCmd &cmd,
                        const native::Pm4Mirror &mirror, RenderPacket &out,
                        std::string &error) {
  error.clear();
  if (!cmd.pm4_capture_ok) {
    error = "PM4 dependency capture failed";
    return false;
  }
  CapturedMemory memory;
  if (!CapturedMemory::Capture(batch, cmd, memory, error))
    return false;
  return DecodeCapturedPacket(batch, cmd, mirror, memory, out, error);
}
const std::vector<VertexAttribute>& Declaration(const CapturedMemory& memory,uint32_t decl) {
  static const std::vector<VertexAttribute> empty;
  if(!decl) return empty;
  const uint32_t count=std::min(U32(memory,Address(decl,0x18)),64u);
  if(!count) return empty;
  auto bytes=memory.ReadUnambiguous(Address(decl,0x34),count*12);
  thread_local VertexDeclarationCache cache;
  if(!bytes.empty()) return cache.Decode(bytes).attributes;
  thread_local std::vector<VertexAttribute> fallback;
  fallback.clear();
  for(uint32_t i=0;i<count;++i) {
    auto a=Address(decl,0x34+uint64_t(i)*12);
    auto stream=U16(memory,a);if(stream==0xff) break;
    fallback.push_back({stream,U16(memory,Address(a,2)),U32(memory,Address(a,4)),
                       memory.Read(Address(a,9),1)[0],memory.Read(Address(a,10),1)[0]});
  }
  return fallback;
}
namespace {
// Device-shadow state for a draw (see state_source.h). In compare mode the
// packet keeps its PM4 values and mismatches are counted per field; in device
// mode the device values replace them.
struct StateCompareCounters {
  std::atomic<uint64_t> draws{0}, uncaptured{0}, vs{0}, ps{0}, bools{0}, loops{0}, poly{0},
      scissor{0}, fetch{0};
  std::atomic<int64_t> last_log{0};
};
StateCompareCounters state_compare;

void ApplyDeviceState(DrawPacket &d, uint32_t dev, const CapturedMemory &memory) {
  const auto &layout = native::profile::kDevice;
  const auto constants = memory.ReadUnambiguous(Address(dev, layout.vs_constants), 2048 * 4);
  const auto scissor_bytes = memory.ReadUnambiguous(Address(dev, kDeviceScissor), 12);
  const bool compare = StateCompare() && !StateFromDevice();
  auto &c = state_compare;
  ++c.draws;
  if (constants.empty() || scissor_bytes.empty()) {
    ++c.uncaptured;
    return;
  }
  std::array<uint32_t, 1024> vs, ps;
  // A float constant the game never set keeps the XDK shadow's initial
  // 0xFFFFFFFF; the XDK never sends it, so the GPU reads its reset value 0.
  for (uint32_t i = 0; i < 1024; ++i) {
    vs[i] = CapturedWord(constants, i * 4);
    ps[i] = CapturedWord(constants, 4096 + i * 4);
    if (vs[i] == 0xFFFFFFFFu) vs[i] = 0;
    if (ps[i] == 0xFFFFFFFFu) ps[i] = 0;
  }
  std::array<uint32_t, 4> poly;
  for (uint32_t i = 0; i < 4; ++i) poly[i] = d.registers[0x380 + i];
  const uint32_t offset = CapturedWord(scissor_bytes, 0), tl = CapturedWord(scissor_bytes, 4),
                 br = CapturedWord(scissor_bytes, 8);
  const Rect scissor = DecodeScissor({tl, br, offset, d.registers[0xe], d.registers[0xf]}, true,
                                     d.tiling_active, 1);
  std::array<std::array<uint32_t, 6>, 32> fetch;
  const auto fetch_bytes = memory.ReadUnambiguous(Address(dev, layout.fetch_constants), 32 * 24);
  for (uint32_t s = 0; s < 32; ++s)
    for (uint32_t i = 0; i < 6; ++i)
      fetch[s][i] = fetch_bytes.empty() ? d.texture_fetch[s][i] : CapturedWord(fetch_bytes, s * 24 + i * 4);
  std::array<uint32_t, 40> bool_loop;
  const auto bl = memory.ReadUnambiguous(Address(dev, layout.vs_bools), 40 * 4);
  for (uint32_t i = 0; i < 40; ++i) {
    uint32_t packet_value;
    std::memcpy(&packet_value, d.constants.shared.data() + (i < 8 ? 256 + i * 4 : 304 + (i - 8) * 4), 4);
    bool_loop[i] = bl.empty() ? packet_value : CapturedWord(bl, i * 4);
  }
  if (compare) {
    static std::atomic<uint32_t> details[3]{};
    auto detail = [&](const char *what, uint32_t index, uint32_t pm4, uint32_t device) {
      if (details[what[0] == 'v' ? 0 : what[0] == 'p' ? 1 : 2].fetch_add(1) < 15)
        REXLOG_INFO("native state compare detail: {} index={} pm4={:08X} device={:08X} prim={} count={} serial={}",
                    what, index, pm4, device, int(d.primitive), d.count, d.command_serial);
    };
    if (vs != d.constants.vs) {
      ++c.vs;
      for (uint32_t i = 0; i < 1024; ++i)
        if (vs[i] != d.constants.vs[i]) { detail("vs", i, d.constants.vs[i], vs[i]); break; }
    }
    if (ps != d.constants.ps) {
      ++c.ps;
      for (uint32_t i = 0; i < 1024; ++i)
        if (ps[i] != d.constants.ps[i]) { detail("ps", i, d.constants.ps[i], ps[i]); break; }
    }
    // Inline (UP) draws: the XDK points vertex fetch slot 31 at the ring copy in
    // PM4 only; the renderer binds the captured inline vertices instead.
    if (fetch != d.texture_fetch && !d.inline_vertices)
      for (uint32_t s = 0; s < 32; ++s)
        for (uint32_t i = 0; i < 6; ++i)
          if (fetch[s][i] != d.texture_fetch[s][i]) {
            detail(d.inline_vertices ? "fetch(inline)" : "fetch", s * 6 + i, d.texture_fetch[s][i], fetch[s][i]);
            s = 32;
            break;
          }
    for (uint32_t i = 0; i < 40; ++i) {
      uint32_t packet_value;
      std::memcpy(&packet_value, d.constants.shared.data() + (i < 8 ? 256 + i * 4 : 304 + (i - 8) * 4), 4);
      if (packet_value != bool_loop[i]) { ++(i < 8 ? c.bools : c.loops); break; }
    }
    if (poly != d.polygon_offset_registers) ++c.poly;
    if (std::memcmp(&scissor, &d.scissor, sizeof(Rect)) != 0) ++c.scissor;
    if (fetch != d.texture_fetch) ++c.fetch;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = c.last_log.load();
    if (now - prev >= 10 && c.last_log.compare_exchange_strong(prev, now))
      REXLOG_INFO("native state compare: draws={} uncaptured={} differ vs={} ps={} bools={} loops={} poly={} scissor={} fetch={}",
                  c.draws.load(), c.uncaptured.load(), c.vs.load(), c.ps.load(), c.bools.load(),
                  c.loops.load(), c.poly.load(), c.scissor.load(), c.fetch.load());
    return;
  }
  d.constants.vs = vs;
  d.constants.ps = ps;
  for (uint32_t i = 0; i < 40; ++i)
    std::memcpy(d.constants.shared.data() + (i < 8 ? 256 + i * 4 : 304 + (i - 8) * 4), &bool_loop[i], 4);
  d.polygon_offset_registers = poly;
  d.scissor = scissor;
  d.texture_fetch = fetch;
}

// Replay and decoding share the same owned snapshot. Packets retain it after
// the worker recycles the source batch; no live guest reads are introduced.
bool DecodeCapturedPacket(const WorkBatch &batch, const WorkCmd &cmd,
                          const native::Pm4Mirror &mirror,
                          const CapturedMemory &memory, RenderPacket &out,
                          std::string &error,DrawPacketOwner storage) {
  try {
    switch (cmd.op) {
    case Op::kDraw:
    case Op::kDrawIndexed:
    case Op::kDrawInline: {
      auto owned=storage.payload?std::move(storage):DrawPacketOwner(std::make_shared<DrawPacket>());
      auto& d=*owned.payload;
      d.memory = memory;
      d.pass = cmd.pass;
      // The XDK device slots can be reversed during internal passes. Match
      // the reference renderer: the owned container determines the stage.
      for(auto& shader:{cmd.vertex_shader,cmd.pixel_shader}) if(shader) (shader->vertex?d.vertex_shader:d.pixel_shader)=shader;
      d.textures = cmd.textures;
      d.texture_errors = cmd.texture_errors;
      d.command_serial = cmd.command_serial;
      d.tiling_active = cmd.tiling_active;
      if (!DecodePrimitive(cmd.u[0], d.primitive, error))
        return false;
      d.indexed = cmd.op == Op::kDrawIndexed;
      d.inline_vertices = cmd.op == Op::kDrawInline;
      d.first = d.indexed ? cmd.u[2] : (d.inline_vertices ? 0 : cmd.u[1]);
      d.count = d.indexed ? cmd.u[3] : cmd.u[2];
      d.base_vertex = d.indexed ? int32_t(cmd.u[1]) : 0;
      d.constants.vs=mirror.FloatConstants(false);
      d.constants.ps=mirror.FloatConstants(true);
      for (uint32_t i = 0; i < 8; ++i) {
        auto v = mirror.reg(native::Pm4Mirror::kBoolConstantBase + i);
        std::memcpy(d.constants.shared.data() + 256 + i * 4, &v, 4);
      }
      for (uint32_t i = 0; i < 32; ++i) {
        auto v = mirror.reg(native::Pm4Mirror::kLoopConstantBase + i);
        std::memcpy(d.constants.shared.data() + 304 + i * 4, &v, 4);
      }
      for (uint32_t i = 0; i < d.polygon_offset_registers.size(); ++i)
        d.polygon_offset_registers[i] = mirror.reg(0x2380 + i);
      d.scissor = DecodeScissor({mirror.reg(0x2081), mirror.reg(0x2082),
                                 mirror.reg(0x2080), mirror.reg(0x200e),
                                 mirror.reg(0x200f)},
                                true, d.tiling_active, 1);
      if (cmd.device)
        DeviceState(d, cmd.device, mirror);
      if (cmd.device) {
        const auto &layout = native::profile::kDevice;
        for (uint32_t i = 0; i < 8; ++i) {
          auto reg = native::Pm4Mirror::kBoolConstantBase + i;
          auto value =
              mirror.written(reg)
                  ? mirror.reg(reg)
                  : U32(memory, Address(cmd.device, layout.vs_bools + i * 4));
          std::memcpy(d.constants.shared.data() + 256 + i * 4, &value, 4);
        }
        for (uint32_t i = 0; i < 32; ++i) {
          auto reg = native::Pm4Mirror::kLoopConstantBase + i;
          uint32_t offset =
              i < 16 ? layout.vs_loops + i * 4 : layout.ps_loops + (i - 16) * 4;
          auto value = mirror.written(reg)
                           ? mirror.reg(reg)
                           : U32(memory, Address(cmd.device, offset));
          std::memcpy(d.constants.shared.data() + 304 + i * 4, &value, 4);
        }
      }
      if (cmd.device && (StateFromDevice() || StateCompare()))
        ApplyDeviceState(d, cmd.device, memory);
      if (uint64_t(cmd.stream_first) + cmd.stream_count > batch.streams.size())
        throw std::out_of_range("Command stream range exceeds batch");
      if (!cmd.streams_ok)
        throw std::out_of_range("Guest stream capture failed");
      for (uint64_t i = cmd.stream_first;
           i < uint64_t(cmd.stream_first) + cmd.stream_count; ++i) {
        const auto &s = batch.streams[size_t(i)];
        d.streams.push_back(
            {s.stream, s.offset, s.size, s.stride, Buffer(memory, s.buffer)});
        auto decl = s.buffer.decl;
        if (!decl)
          continue;
        for (const auto& attribute : Declaration(memory,decl)) {
          if(attribute.stream!=s.stream) continue;
          auto usage=attribute.usage,index=attribute.index;
          auto offset=attribute.offset,type=attribute.type;
          d.attributes.push_back({s.stream, offset, type, usage, index});
          if (usage < 14 && index < 16)
            d.vertex_fetch[usage * 16 + index] = {
                s.stream, Address(s.offset, offset), s.stride, type};
        }
      }
      static_assert(sizeof(d.vertex_fetch)==sizeof(d.constants.shared)-512,
                    "Replay storage requires a complete shared metadata suffix");
      std::memcpy(d.constants.shared.data() + 512, d.vertex_fetch.data(),
                  sizeof(d.vertex_fetch));
      for(auto& stream:d.streams) {
        if(stream.update.bytes.empty()) continue;
        uint32_t stride=stream.stride;
        if(!stride) {error="Captured vertex update has zero stride";return false;}
        uint32_t phase=uint32_t((uint64_t(stream.update.plan.phase)+stride-
                                stream.update.plan.begin%stride)%stride);
        SwapVertexElements(stream.update.bytes,stride,phase,stream.stream,d.attributes);
      }
      if (d.inline_vertices) {
        d.inline_stride = cmd.u[3];
        uint64_t size = uint64_t(d.count) * d.inline_stride;
        if (size > UINT32_MAX)
          throw std::out_of_range("Inline vertex range overflow");
        auto bytes = memory.Read(cmd.u[1], uint32_t(size));
        d.inline_data.assign(bytes.begin(), bytes.end());
        if (cmd.device) {
          auto decl =
              U32(memory,
                  Address(cmd.device, native::profile::kDevice.vertex_decl));
          for (const auto& attribute : Declaration(memory,decl)) {
            auto stream=attribute.stream,offset=attribute.offset,type=attribute.type;
            auto usage=attribute.usage,index=attribute.index;
            d.attributes.push_back({stream, offset, type, usage, index});
            if (usage < 14 && index < 16)
              d.vertex_fetch[usage * 16 + index] = {stream, offset,
                                                    d.inline_stride, type};
          }
          std::memcpy(d.constants.shared.data() + 512, d.vertex_fetch.data(),
                      sizeof(d.vertex_fetch));
        }
        SwapVertexElements(d.inline_data,d.inline_stride,0,UINT32_MAX,d.attributes);
      }
      if (d.indexed) {
        d.primitive_restart=cmd.index.reset_index!=UINT32_MAX;
        if (!cmd.has_index)
          throw std::out_of_range("Guest index capture failed");
        d.indices = Buffer(memory, cmd.index);
        if (!d.indices.bytes.empty()) {
          uint32_t width = cmd.index32 ? 4 : 2;
          if (d.indices.bytes.size() % width || cmd.index.begin % width)
            throw std::out_of_range("Unaligned captured indices");
          // Normalize only the captured update; unchanged buffer data stays in
          // the backend cache. Quads are expanded from the actual draw slice
          // below.
          auto primitive = d.primitive == Primitive::kQuads
                               ? Primitive::kTriangles
                               : d.primitive;
          if (!NormalizeIndices(
                  d.indices.bytes, 0, uint32_t(d.indices.bytes.size() / width),
                  {cmd.index32, cmd.index.format >> 2, cmd.index.reset_index},
                  primitive, d.indices.normalized, error))
            return false;
        }
        if (d.primitive == Primitive::kQuads && d.count && !cmd.u[5]) {
          error = "Indexed quads have no captured expansion";
          return false;
        }
        if (d.primitive == Primitive::kQuads && cmd.u[5]) {
          if (uint64_t(cmd.u[4]) + uint64_t(cmd.u[5]) * 4 > batch.bytes.size())
            throw std::out_of_range("Captured expanded quads exceed batch");
          d.expanded_indices.resize(cmd.u[5]);
          std::memcpy(d.expanded_indices.data(),
                      batch.bytes.data() + cmd.u[4], size_t(cmd.u[5]) * 4);
          d.first = 0;
          d.count = cmd.u[5];
          d.primitive = Primitive::kTriangles;
        }
      }
      FinalizeDrawConstants(d);
      out = std::move(owned);
      return true;
    }
    case Op::kClear: {
      ClearPacket c{
          cmd.u[2], {}, cmd.f, cmd.u[7], Rects(memory, cmd.u[1], cmd.u[0]),
          memory};
      std::memcpy(c.color.data(), cmd.u + 3, 16);
      if(cmd.device) {
        const auto& layout=native::profile::kDevice;
        for(uint32_t i=0;i<4;++i) c.colors[i]=Surface(memory,U32(memory,Address(cmd.device,layout.render_targets+i*4)),false);
        c.depth_surface=Surface(memory,U32(memory,Address(cmd.device,layout.depth_stencil)),true);
      }
      out = std::move(c);
      return true;
    }
    case Op::kResolve: {
      ResolvePacket r{};
      r.command_serial = cmd.command_serial;
      r.flags = cmd.u[0];
      r.has_source_rect=cmd.u[1]!=0;r.has_destination_point=cmd.u[3]!=0;r.tiling_active=cmd.tiling_active;
      r.destination = cmd.u[2];
      r.has_copy_draw=cmd.resolve_copy_draw;r.copy_dest_swap=bool((cmd.resolve_copy_dest_info>>24)&1);
      r.memory = memory;
      if (cmd.u[1])
        r.source = Rects(memory, cmd.u[1], 1).front();
      if (cmd.u[3])
        r.destination_point = {int32_t(U32(memory, cmd.u[3])),
                               int32_t(U32(memory, Address(cmd.u[3], 4)))};
      r.clear_color = Color(memory, cmd.u[4]);
      r.clear_depth = cmd.f;
      r.clear_stencil = cmd.u[5];
      r.level=cmd.u[6];r.slice=cmd.u[7];
      if(cmd.device) {
        const auto& layout=native::profile::kDevice;
        uint32_t source=cmd.u[0]&7;
        if(source>4) {error="Unsupported resolve source slot";return false;}
        uint32_t offset=source==4?layout.depth_stencil:layout.render_targets+source*4;
        r.source_surface=Surface(memory,U32(memory,Address(cmd.device,offset)),source==4);
        if(r.flags&0x100) r.clear_color_surface=Surface(memory,U32(memory,Address(cmd.device,layout.render_targets)),false);
        if(r.flags&0x200) r.clear_depth_surface=Surface(memory,U32(memory,Address(cmd.device,layout.depth_stencil)),true);
      }
      if(r.destination)
        for(uint32_t i=0;i<6;++i) r.destination_fetch[i]=U32(memory,Address(r.destination,0x1c+i*4));
      out = std::move(r);
      return true;
    }
    case Op::kSwap:
      out = SwapPacket{cmd.u[0], cmd.u64, memory,cmd.gamma,cmd.gamma_enabled};
      return true;
    case Op::kBeginTiling: {
      PassPacket packet{cmd.op,
                       cmd.pass,
                       Rects(memory, cmd.u[1], cmd.u[0]),
                       Color(memory, cmd.u[2]),
                       cmd.f,
                       cmd.u[3]};
      packet.clear_color=cmd.u[2]!=0;
      if(cmd.device) {
        packet.color_surface=Surface(memory,U32(memory,Address(cmd.device,native::profile::kDevice.render_targets)),false);
        packet.depth_surface=Surface(memory,U32(memory,Address(cmd.device,native::profile::kDevice.depth_stencil)),true);
      }
      out=std::move(packet);
      return true;
    }
    case Op::kEndTiling:
    case Op::kPassEnd:
    case Op::kRing:
      out = PassPacket{
          cmd.op, cmd.op == Op::kPassEnd ? int(cmd.u[0]) : cmd.pass, {}, {}, 0,
          0};
      return true;
    }
    error = "Unknown captured render operation";
    return false;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace
} // namespace superman_returns::graphics::guest
