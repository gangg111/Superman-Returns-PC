#include "../../port/src/graphics/guest/captured_batch.h"
#include "../../port/src/graphics/guest/capture_batch_pool.h"
#include "../../port/src/graphics/guest/pm4_capture.h"
#include "../../port/src/graphics/guest/render_packet.h"
#include "../../port/src/native_renderer/pm4_mirror.h"
#include "../../port/src/native_renderer/game_profile.h"
#include "test_main.h"
SR_TEST(packet_check_cadence_limits_debug_capture) {
  using superman_returns::graphics::guest::IsCaptureFrame;
  SR_CHECK(IsCaptureFrame(0,120));SR_CHECK(!IsCaptureFrame(119,120));
  SR_CHECK(IsCaptureFrame(120,120));SR_CHECK(IsCaptureFrame(7,1));
  SR_CHECK(IsCaptureFrame(7,0));
}
#include <bit>
#include <algorithm>
#include <stdexcept>
using namespace superman_returns::graphics::guest;
SR_TEST(draw_packet_keeps_replay_payload_below_24_kib) {
  // Bound per-draw memory independently of arena allocation count. At 3,008
  // draws, 4 KiB of unused state would add nearly 12 MiB to every batch.
  SR_CHECK(sizeof(DrawPacket)<=24*1024);
  std::printf("draw_packet_bytes=%zu\n",sizeof(DrawPacket));
}
SR_TEST(replay_preserves_polygon_offset_register_bits) {
  WorkBatch batch;WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;cmd.u[2]=3;
  superman_returns::native::Pm4Mirror mirror;
  const uint8_t words[]={0,3,0x23,0x80,0x42,0,0,0,0x33,0x80,0,1,
                        0x42,0x80,0,0,0xb4,0,0,1};
  mirror.ScanCopy(nullptr,words,sizeof(words));
  DrawPacketArena arena(1,true);RenderPacket ordinary,replay;std::string error;
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,ordinary,error));
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,replay,error,arena.Next()));
  for(auto* draw:{GetDraw(ordinary),GetDraw(replay)}) {
    for(uint32_t i=0;i<4;++i) SR_CHECK_EQ(draw->polygon_offset_registers[i],mirror.reg(0x2380+i));
    draw->registers[0x205]=1<<11;FinalizeDrawConstants(*draw);
    SR_CHECK_EQ(draw->depth_bias,1);SR_CHECK_EQ(draw->slope_bias,2.0f);
    draw->registers[0x205]=1<<12;FinalizeDrawConstants(*draw);
    SR_CHECK_EQ(draw->depth_bias,-2);SR_CHECK_EQ(draw->slope_bias,4.0f);
  }
}
SR_TEST(replay_probe_does_not_process_a_rejected_capture) {
  WorkBatch batch; WorkCmd cmd; cmd.pm4_capture_ok=false;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket packet; std::string error; ReplayProbe probe;
  SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,packet,error,{}, {},&probe));
  SR_CHECK_EQ(probe.capture_ns,0u);
  SR_CHECK_EQ(probe.pm4_ns,0u);
  SR_CHECK_EQ(probe.decode_ns,0u);
}
SR_TEST(replay_storage_matches_initialized_draw_after_complete_decode) {
  ConstantSnapshot defaults;
  for(auto v:defaults.vs) SR_CHECK_EQ(v,0u);
  for(auto v:defaults.ps) SR_CHECK_EQ(v,0u);
  for(auto v:defaults.shared) SR_CHECK_EQ(v,0u);
  WorkBatch batch; WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;cmd.u[2]=3;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket ordinary,prepared;std::string error;
  // Write nonzero words, including NaN payloads, into both banks and state.
  const uint8_t words[]={0,0,0x40,0,0x7f,0xc0,0,1,
                         0,0,0x44,0,0x3f,0x80,0,0,
                         0,0,0x20,0,0x12,0x34,0x56,0x78};
  mirror.ScanCopy(nullptr,words,sizeof(words));
  DrawPacketArena arena(3,true);
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,ordinary,error));
  auto storage=arena.Next();
  // Decode must assign the complete suffix, even when there are no attributes.
  std::fill(storage.payload->constants.shared.begin()+512,storage.payload->constants.shared.end(),0xa5);
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,prepared,error,std::move(storage)));
  const auto& a=*GetDraw(ordinary);const auto& b=*GetDraw(prepared);
  SR_CHECK(a.constants.vs==b.constants.vs);SR_CHECK(a.constants.ps==b.constants.ps);
  SR_CHECK(a.constants.shared==b.constants.shared);
  SR_CHECK(a.registers==b.registers);SR_CHECK(a.polygon_offset_registers==b.polygon_offset_registers);
  SR_CHECK(a.texture_fetch==b.texture_fetch);
  SR_CHECK_EQ(std::memcmp(a.vertex_fetch.data(),b.vertex_fetch.data(),sizeof(a.vertex_fetch)),0);
  cmd.u[0]=UINT32_MAX;
  auto* published=GetDraw(prepared);
  SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,prepared,error,arena.Next()));
  SR_CHECK(GetDraw(prepared)==published);
  // A late failure happens after the metadata copy, and still publishes nothing.
  cmd.op=Op::kDrawIndexed;cmd.u[0]=4;cmd.u[3]=3;cmd.has_index=false;
  SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,prepared,error,arena.Next()));
  SR_CHECK(GetDraw(prepared)==published);SR_CHECK(GetDraw(prepared)->constants.shared==a.constants.shared);
}
SR_TEST(pm4_sequential_constant_writes_match_individual_writes) {
  using superman_returns::native::Pm4Mirror;
  auto put=[](std::vector<uint8_t>& bytes,uint32_t word) {
    bytes.push_back(uint8_t(word>>24));bytes.push_back(uint8_t(word>>16));
    bytes.push_back(uint8_t(word>>8));bytes.push_back(uint8_t(word));
  };
  for(auto [first,count]:std::array<std::pair<uint32_t,uint32_t>,6>{{
      {0x4000,1024},{0x4400,1024},{0x43f0,32},{0x3ff0,32},{0x47f0,32},{0x4ff0,32}}}) {
    Pm4Mirror bulk,individual;
    for(uint32_t round=0;round<3;++round) {
      std::vector<uint8_t> stream,scalar;
      put(stream,((count-1)<<16)|first);
      for(uint32_t i=0;i<count;++i) {
        // Repeated writes, changed values, NaNs and infinities remain raw here.
        uint32_t value=i%4==0?0x7fc00001u:(i%4==1?0x7f800000u:0x3f800000u+i+(round==2));
        put(stream,value);put(scalar,(first+i)|0x8000);put(scalar,value);
      }
      bulk.ScanCopy(nullptr,stream.data(),uint32_t(stream.size()));
      individual.ScanCopy(nullptr,scalar.data(),uint32_t(scalar.size()));
      for(uint32_t i=0;i<Pm4Mirror::kRegisterCount;++i) {
        SR_CHECK_EQ(bulk.reg(i),individual.reg(i));
        SR_CHECK_EQ(bulk.written(i),individual.written(i));
      }
      SR_CHECK_EQ(bulk.vs_version,individual.vs_version);
      SR_CHECK_EQ(bulk.ps_version,individual.ps_version);
      SR_CHECK(bulk.FloatConstants(false)==individual.FloatConstants(false));
      SR_CHECK(bulk.FloatConstants(true)==individual.FloatConstants(true));
    }
  }
}
SR_TEST(pm4_constant_packet_variants_preserve_both_bank_versions) {
  using superman_returns::native::Pm4Mirror;
  auto put=[](std::vector<uint8_t>& bytes,uint32_t word) {
    for(int shift=24;shift>=0;shift-=8) bytes.push_back(uint8_t(word>>shift));
  };
  constexpr uint32_t first=0x43fc,count=8;
  std::vector<uint8_t> values;
  for(uint32_t i=0;i<count;++i) put(values,0x3f800000+i);
  for(uint32_t opcode: {0x2du,0x55u,0x56u,0x2fu}) {
    Pm4Mirror actual,expected;
    std::vector<uint8_t> stream,scalar;
    if(opcode==0x2f) {
      put(stream,0xc0000000u|(2u<<16)|(opcode<<8));
      put(stream,0x200);put(stream,first-0x4000);put(stream,count);
    } else {
      put(stream,0xc0000000u|(count<<16)|(opcode<<8));
      put(stream,opcode==0x2d?first-0x4000:first);
      stream.insert(stream.end(),values.begin(),values.end());
    }
    put(scalar,((count-1)<<16)|first);
    scalar.insert(scalar.end(),values.begin(),values.end());
    for(unsigned round=0;round<2;++round) {
      actual.ScanCopyUsing(stream.data(),uint32_t(stream.size()),[&](uint32_t address,uint32_t bytes)->std::span<const uint8_t> {
        SR_CHECK_EQ(address,0xa0000200u);SR_CHECK_EQ(bytes,values.size());return values;
      });
      expected.ScanCopy(nullptr,scalar.data(),uint32_t(scalar.size()));
      SR_CHECK_EQ(actual.vs_version,expected.vs_version);
      SR_CHECK_EQ(actual.ps_version,expected.ps_version);
      SR_CHECK(actual.FloatConstants(false)==expected.FloatConstants(false));
      SR_CHECK(actual.FloatConstants(true)==expected.FloatConstants(true));
      for(uint32_t i=first;i<first+count;++i) SR_CHECK_EQ(actual.written(i),expected.written(i));
    }
  }
}
SR_TEST(draw_payload_survives_queue_growth_without_relocation) {
  WorkBatch batch;
  batch.bytes={1,2,3,4};batch.ranges={{0x1000,4,0}};
  WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;cmd.range_count=1;
  auto shader=std::make_shared<ShaderCapture>();shader->vertex=true;shader->hash=123;
  cmd.vertex_shader=shader;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket packet;std::string error;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,packet,error));
  const auto* payload=GetDraw(packet);
  SR_CHECK(payload);
  std::vector<RenderPacket> queue;
  queue.push_back(std::move(packet));
  for(unsigned i=0;i<128;++i) queue.emplace_back(PassPacket{});
  batch.Clear();cmd.vertex_shader.reset();shader.reset();
  const auto* retained=GetDraw(queue.front());
  SR_CHECK(retained==payload);
  SR_CHECK_EQ(retained->memory.Read(0x1000,4)[2],3);
  SR_CHECK_EQ(retained->vertex_shader->hash,123u);
  SR_CHECK(sizeof(RenderPacket)<=1024);
}
SR_TEST(failed_draw_decode_preserves_published_packet) {
  WorkBatch batch;WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=UINT32_MAX;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket packet=SwapPacket{77,88,{}};std::string error;
  SR_CHECK(!DecodeRenderPacket(batch,cmd,mirror,packet,error));
  SR_CHECK(std::holds_alternative<SwapPacket>(packet));
  SR_CHECK_EQ(std::get<SwapPacket>(packet).frontbuffer,77u);
}
SR_TEST(batch_draw_payloads_remain_distinct_after_arena_release) {
  static_assert(!std::is_copy_constructible_v<RenderPacket>);
  static_assert(!std::is_copy_constructible_v<DrawPacketArena>);
  WorkBatch batch;WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket first,second;std::string error;
  {
    DrawPacketArena arena(2);
    cmd.u[2]=3;SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,first,error,arena.Next()));
    cmd.u[2]=6;SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,second,error,arena.Next()));
    bool exhausted=false;
    try {arena.Next();} catch(const std::out_of_range&) {exhausted=true;}
    SR_CHECK(exhausted);
  }
  SR_CHECK(GetDraw(first)!=GetDraw(second));
  SR_CHECK_EQ(GetDraw(first)->count,3u);
  SR_CHECK_EQ(GetDraw(second)->count,6u);
  first=PassPacket{};
  SR_CHECK_EQ(GetDraw(second)->count,6u);
}
SR_TEST(recycled_arena_memory_gives_fresh_packets) {
  // The pool hands the same memory to the next arena: every packet must start as a new one.
  DrawPacket* used=nullptr;
  {
    DrawPacketArena arena(4);
    auto owner=arena.Next();
    used=owner.payload.get();
    used->count=99;used->registers[0x104]=15;used->constants.vs[7]=1234;used->streams.resize(3);
    used->textures[2]=std::shared_ptr<const TextureCapture>(std::make_shared<int>(1),nullptr);  // an owner only
    used->command_serial=5;
  }
  DrawPacketArena again(4);
  auto owner=again.Next();
  DrawPacket* fresh=owner.payload.get();
  SR_CHECK(fresh==used);  // the recycled memory, not a new block
  SR_CHECK_EQ(fresh->count,0u);
  SR_CHECK_EQ(fresh->registers[0x104],0u);
  SR_CHECK_EQ(fresh->constants.vs[7],0u);
  SR_CHECK(fresh->streams.empty());
  SR_CHECK_EQ(fresh->textures[2].use_count(),0);
  SR_CHECK_EQ(fresh->command_serial,0u);
}
SR_TEST(a_packet_outlives_its_arena_and_lets_its_texture_go_with_it) {
  std::weak_ptr<int> texture;  // stands for the texture capture's lifetime
  DrawPacketOwner kept;
  {
    DrawPacketArena arena(2);
    kept=arena.Next();
    auto capture=std::make_shared<int>(1);texture=capture;
    kept.payload->textures[0]=std::shared_ptr<const TextureCapture>(std::move(capture),nullptr);
  }
  SR_CHECK(!texture.expired());  // the arena is gone, the packet and its texture are not
  kept={};
  SR_CHECK(texture.expired());   // the last alias let the packets (and the texture) go at once
}
SR_TEST(pm4_dependency_capture_preserves_caller_state_between_commands) {
  WorkBatch batch;
  // Type0 writes to VS c0.x, then PS c0.x in a later command.
  batch.bytes = {0, 0, 0x40, 0, 0x3f, 0x80, 0, 0,
                 0, 0, 0x44, 0, 0x40, 0, 0, 0};
  WorkCmd cmd;
  cmd.ring_bytes = 8;
  superman_returns::native::Pm4Mirror mirror;
  std::string error;
  auto no_dependencies = [](uint32_t, uint32_t) -> std::span<const uint8_t> {
    SR_CHECK(false); return {};
  };
  SR_CHECK(CapturePm4Dependencies(batch, cmd, no_dependencies, error, &mirror));
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase), 0x3f800000u);
  cmd.ring_offset = 8;
  SR_CHECK(CapturePm4Dependencies(batch, cmd, no_dependencies, error, &mirror));
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase), 0x3f800000u);
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase + 1024), 0x40000000u);
  SR_CHECK_EQ(mirror.packets, 2u);
  SR_CHECK(batch.ranges.empty());
}
SR_TEST(pm4_dependency_capture_owns_nested_sources) {
  WorkBatch batch;
  batch.bytes = {0xc0, 0x01, 0x3f, 0, 0, 0, 2, 0, 0, 0, 0, 4};
  WorkCmd cmd;
  cmd.ring_bytes = 12;
  std::vector<uint8_t> indirect = {0xc0, 0x02, 0x2f, 0, 0, 0, 1, 0,
                                   0,    0,    0,    0, 0, 0, 0, 1};
  std::vector<uint8_t> constant = {0x3f, 0x80, 0, 0};
  std::string error;
  SR_CHECK(CapturePm4Dependencies(
      batch, cmd,
      [&](uint32_t address, uint32_t length) -> std::span<const uint8_t> {
        if (address == 0xa0000200 && length == 16)
          return indirect;
        if (address == 0xa0000100 && length == 4)
          return constant;
        return {};
      },
      error));
  cmd.range_count = uint32_t(batch.ranges.size());
  indirect.clear();
  constant.clear();
  CapturedMemory memory;
  SR_CHECK(CapturedMemory::Capture(batch, cmd, memory, error));
  superman_returns::native::Pm4Mirror mirror;
  mirror.ScanCopyUsing(batch.bytes.data(), cmd.ring_bytes,
                       [&](uint32_t address, uint32_t length) {
                         return memory.Read(address, length);
                       });
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase), 0x3f800000u);
}
SR_TEST(pm4_dependency_capture_freezes_reused_reader_storage_before_nested_reads) {
  WorkBatch batch;
  batch.bytes={0xc0,0x01,0x3f,0,0,0,2,0,0,0,0,4,
               0,0,0x44,0,0x40,0,0,0}; // PS write after returning from the indirect.
  batch.bytes.shrink_to_fit();
  WorkCmd cmd;cmd.ring_bytes=uint32_t(batch.bytes.size());
  std::vector<uint8_t> scratch;
  scratch.reserve(16);
  superman_returns::native::Pm4Mirror mirror;std::string error;
  SR_CHECK(CapturePm4Dependencies(batch,cmd,[&](uint32_t address,uint32_t length)->std::span<const uint8_t> {
    if(address==0xa0000200 && length==16) scratch={0xc0,0x02,0x2f,0,0,0,1,0,0,0,0,0,0,0,0,1};
    else if(address==0xa0000100 && length==4) scratch={0x3f,0x80,0,0};
    else return {};
    return scratch;
  },error,&mirror));
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase),0x3f800000u);
  SR_CHECK_EQ(mirror.reg(mirror.kAluConstantBase+1024),0x40000000u);
  SR_CHECK_EQ(batch.ranges.size(),2u);
  SR_CHECK_EQ(batch.ranges[0].offset,20u);
  SR_CHECK_EQ(batch.ranges[1].offset,36u);
  SR_CHECK_EQ(batch.bytes[20],0xc0u); // reader scratch was overwritten, owned indirect was not.
  SR_CHECK_EQ(batch.bytes[36],0x3fu);
}
// Same ring as pm4_dependency_capture_owns_nested_sources: an indirect buffer
// (16 bytes) that in turn loads a 4-byte ALU constant, so two guest reads.
SR_TEST(pm4_capture_probe_counts_reads_and_changes_nothing) {
  const std::vector<uint8_t> ring = {0xc0, 0x01, 0x3f, 0, 0, 0, 2, 0, 0, 0, 0, 4};
  const std::vector<uint8_t> indirect = {0xc0, 0x02, 0x2f, 0, 0, 0, 1, 0,
                                         0,    0,    0,    0, 0, 0, 0, 1};
  const std::vector<uint8_t> constant = {0x3f, 0x80, 0, 0};
  auto reader = [&](uint32_t address, uint32_t length) -> std::span<const uint8_t> {
    if (address == 0xa0000200 && length == 16) return indirect;
    if (address == 0xa0000100 && length == 4) return constant;
    return {};
  };
  WorkCmd cmd;
  cmd.ring_bytes = uint32_t(ring.size());

  WorkBatch plain;
  plain.bytes.assign(ring.begin(), ring.end());
  std::string plain_error;
  const bool plain_ok = CapturePm4Dependencies(plain, cmd, reader, plain_error);

  WorkBatch probed;
  probed.bytes.assign(ring.begin(), ring.end());
  std::string probed_error;
  Pm4CaptureProbe probe;
  const bool probed_ok =
      CapturePm4Dependencies(probed, cmd, reader, probed_error, nullptr, &probe);

  SR_CHECK(plain_ok);
  SR_CHECK_EQ(probed_ok, plain_ok);
  SR_CHECK(plain_error == probed_error);
  SR_CHECK(plain.bytes == probed.bytes);
  SR_CHECK_EQ(plain.ranges.size(), 2u);
  SR_CHECK_EQ(probed.ranges.size(), plain.ranges.size());
  for (size_t i = 0; i < plain.ranges.size(); ++i) {
    SR_CHECK_EQ(probed.ranges[i].address, plain.ranges[i].address);
    SR_CHECK_EQ(probed.ranges[i].length, plain.ranges[i].length);
    SR_CHECK_EQ(probed.ranges[i].offset, plain.ranges[i].offset);
  }
  SR_CHECK_EQ(probe.reads, 2u);
  SR_CHECK_EQ(probe.bytes, 20u);
  SR_CHECK(probe.copy_ns > 0 || probe.read_ns > 0 || probe.primary_ns > 0);

  // A failing dependency: same error and rollback with and without the probe;
  // only successful reads are counted.
  auto missing_constant = [&](uint32_t address, uint32_t length) -> std::span<const uint8_t> {
    if (address == 0xa0000200 && length == 16) return indirect;
    return {};
  };
  WorkBatch plain_fail, probed_fail;
  plain_fail.bytes.assign(ring.begin(), ring.end());
  probed_fail.bytes.assign(ring.begin(), ring.end());
  std::string plain_fail_error, probed_fail_error;
  Pm4CaptureProbe fail_probe;
  const bool plain_fail_ok =
      CapturePm4Dependencies(plain_fail, cmd, missing_constant, plain_fail_error);
  const bool probed_fail_ok = CapturePm4Dependencies(
      probed_fail, cmd, missing_constant, probed_fail_error, nullptr, &fail_probe);
  SR_CHECK(!plain_fail_ok);
  SR_CHECK_EQ(probed_fail_ok, plain_fail_ok);
  SR_CHECK(!plain_fail_error.empty());
  SR_CHECK(plain_fail_error == probed_fail_error);
  SR_CHECK(plain_fail.bytes == probed_fail.bytes);
  SR_CHECK_EQ(plain_fail.ranges.size(), probed_fail.ranges.size());
  SR_CHECK_EQ(fail_probe.reads, 1u);
  SR_CHECK_EQ(fail_probe.bytes, 16u);
}
SR_TEST(captured_memory_owns_bytes_after_source_reuse) {
  WorkBatch batch;
  batch.bytes = {1, 2, 3, 4, 5, 6};
  batch.ranges = {{0x1000, 4, 0}, {0x2000, 2, 4}};
  WorkCmd cmd;
  cmd.range_count = 2;
  CapturedMemory memory;
  std::string error;
  SR_CHECK(CapturedMemory::Capture(batch, cmd, memory, error));
  batch.Clear();
  SR_CHECK_EQ(memory.Read(0x1000, 4)[2], 3);
  SR_CHECK_EQ(memory.Read(0x2000, 2)[1], 6);
}
SR_TEST(owned_capture_reuses_immutable_bytes_and_limits_reads_to_its_command) {
  auto batch=std::make_shared<WorkBatch>();batch->bytes={1,2,3,4};
  batch->ranges={{0x1000,2,0},{0x1000,2,2}};
  WorkCmd cmd;cmd.range_count=1;
  CapturedMemory first,second;std::string error;
  SR_CHECK(CapturedMemory::CaptureOwned(batch,cmd,first,error));
  SR_CHECK(first.Read(0x1000,2).data()==batch->bytes.data());
  cmd.range_first=1;
  SR_CHECK(CapturedMemory::CaptureOwned(batch,cmd,second,error));
  std::weak_ptr<const WorkBatch> lifetime=batch;
  batch.reset();SR_CHECK(!lifetime.expired());
  SR_CHECK_EQ(first.Read(0x1000,2)[0],1);
  SR_CHECK_EQ(second.Read(0x1000,2)[0],3);
  first={};SR_CHECK(!lifetime.expired());
  second={};SR_CHECK(lifetime.expired());
}
SR_TEST(batch_pool_recycles_only_after_the_last_packet_capture_is_released) {
  auto pool=std::make_shared<CaptureBatchPool>();
  auto batch=pool->Acquire();const auto* original=batch.get();
  batch->bytes={5,6};batch->ranges={{0x1000,2,0}};
  WorkCmd cmd;cmd.range_count=1;CapturedMemory memory;std::string error;
  auto frozen=pool->Freeze(std::move(batch));
  SR_CHECK(CapturedMemory::CaptureOwned(frozen,cmd,memory,error));
  frozen.reset();auto concurrent=pool->Acquire();SR_CHECK(concurrent.get()!=original);
  SR_CHECK_EQ(memory.Read(0x1000,2)[0],5);
  memory={};auto reused=pool->Acquire();SR_CHECK(reused.get()==original);
  SR_CHECK(reused->bytes.empty());SR_CHECK(reused->ranges.empty());
  reused->bytes={9,8};reused->ranges={{0x2000,2,0}};
  frozen=pool->Freeze(std::move(reused));
  SR_CHECK(CapturedMemory::CaptureOwned(frozen,cmd,memory,error));
  frozen.reset();pool.reset(); // a packet remains usable after its producer shuts down
  SR_CHECK_EQ(memory.Read(0x2000,2)[0],9);
  memory={};
}
SR_TEST(capture_range_overflow_is_rejected) {
  WorkBatch batch;
  batch.bytes = {1, 2, 3, 4};
  WorkCmd cmd;
  cmd.range_count = 1;
  CapturedMemory memory;
  std::string error;
  batch.ranges = {{UINT32_MAX, 4, 0}};
  SR_CHECK(!CapturedMemory::Capture(batch, cmd, memory, error));
  batch.ranges = {{0x1000, 4, UINT32_MAX}};
  SR_CHECK(!CapturedMemory::Capture(batch, cmd, memory, error));
  cmd.range_first = UINT32_MAX;
  SR_CHECK(!CapturedMemory::Capture(batch, cmd, memory, error));
}
SR_TEST(capture_overlap_uses_latest_range_without_live_fallback) {
  WorkBatch batch;
  batch.bytes = {1, 2, 3, 4, 9, 8, 7, 6};
  batch.ranges = {{0x1000, 4, 0}, {0x1000, 4, 4}};
  WorkCmd cmd;
  cmd.range_count = 2;
  CapturedMemory memory;
  std::string error;
  SR_CHECK(CapturedMemory::Capture(batch, cmd, memory, error));
  SR_CHECK_EQ(memory.Read(0x1001, 2)[0], 8);
  bool rejected = false;
  try {
    memory.Read(0x1003, 2);
  } catch (const std::out_of_range &) {
    rejected = true;
  }
  SR_CHECK(rejected);
}
SR_TEST(capture_command_ranges_do_not_leak_other_draws) {
  WorkBatch batch;
  batch.bytes = {1, 2, 3, 4};
  batch.ranges = {{0x1000, 2, 0}, {0x1000, 2, 2}};
  WorkCmd cmd;
  cmd.range_count = 1;
  CapturedMemory first, second;
  std::string error;
  SR_CHECK(CapturedMemory::Capture(batch, cmd, first, error));
  cmd.range_first = 1;
  SR_CHECK(CapturedMemory::Capture(batch, cmd, second, error));
  SR_CHECK_EQ(first.Read(0x1000, 2)[0], 1);
  SR_CHECK_EQ(second.Read(0x1000, 2)[0], 3);
}
SR_TEST(packet_owns_constants_and_buffers_after_guest_mutation) {
  WorkBatch batch;
  batch.bytes = {1, 2, 3, 4};
  batch.ranges = {{0xA0001000, 4, 0}};
  StreamPlan stream;
  stream.stride = 4;
  stream.size = 4;
  stream.buffer = {};
  stream.buffer.address = 0x1000;
  stream.buffer.size = 4;
  stream.buffer.action = 2;
  stream.buffer.end = 4;
  batch.streams.push_back(stream);
  WorkCmd cmd;
  cmd.op = Op::kDraw;
  cmd.u[0] = 4;
  cmd.u[2] = 1;
  cmd.range_count = 1;
  cmd.stream_count = 1;
  auto shader = std::make_shared<ShaderCapture>();
  shader->hash = 0x1234;
  shader->vertex = true;
  shader->container = {1, 2, 3, 4};
  cmd.vertex_shader = shader;
  shader.reset();
  superman_returns::native::Pm4Mirror mirror;
  // Type0 VS c0.x=1 in guest byte order.
  uint8_t words[] = {0, 0, 0x40, 0, 0x3f, 0x80, 0, 0};
  mirror.ScanCopy(nullptr, words, sizeof(words));
  RenderPacket packet;
  std::string error;
  SR_CHECK(DecodeRenderPacket(batch, cmd, mirror, packet, error));
  batch.Clear();
  mirror = superman_returns::native::Pm4Mirror{};
  const auto &draw = (*GetDraw(packet));
  SR_CHECK_EQ(draw.constants.vs[0], 0x3f800000);
  SR_CHECK_EQ(draw.streams[0].update.bytes[2], 3);
  cmd.vertex_shader.reset();
  SR_CHECK_EQ(draw.vertex_shader->hash, 0x1234u);
  SR_CHECK_EQ(draw.vertex_shader->container[2], 3);
}
SR_TEST(strip_and_ushort2_metadata_survive_packet) {
  WorkBatch batch;
  batch.bytes = {0, 0, 0, 1, 0, 0xff, 0xff, 0xfe, 0, 0, 0, 2};
  batch.ranges = {{0xA0001000, 12, 0}};
  WorkCmd cmd;
  cmd.op = Op::kDrawIndexed;
  cmd.u[0] = 6;
  cmd.u[3] = 3;
  cmd.range_count = 1;
  cmd.has_index = true;
  cmd.index32 = true;
  cmd.index.address = 0x1000;
  cmd.index.size = 12;
  cmd.index.action = 2;
  cmd.index.end = 12;
  cmd.index.format = 2 | (2 << 2);
  cmd.index.reset_index = 0xfffffe;
  batch.bytes.resize(12 + 0x40, 0);
  // One POSITION0 declaration, stream0, USHORT2, offset0.
  batch.bytes[12 + 0x1b] = 1;
  batch.bytes[12 + 0x39] = 0x2c;
  batch.bytes[12 + 0x3a] = 0x22;
  batch.bytes[12 + 0x3b] = 0x59;
  batch.ranges.push_back({0x3000, 0x40, 12});
  cmd.range_count = 2;
  cmd.stream_count = 1;
  StreamPlan stream;
  stream.stride = 4;
  stream.buffer.decl = 0x3000;
  batch.streams.push_back(stream);
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket packet;
  std::string error;
  SR_CHECK(DecodeRenderPacket(batch, cmd, mirror, packet, error));
  const auto &draw = (*GetDraw(packet));
  SR_CHECK(draw.indices.normalized ==
           std::vector<uint32_t>({1, UINT32_MAX, 2}));
  DrawPacketArena storage(1,true);RenderPacket replay;
  auto owner=storage.Next();
  std::fill(owner.payload->constants.shared.begin()+512,owner.payload->constants.shared.end(),0xa5);
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,replay,error,std::move(owner)));
  SR_CHECK(GetDraw(replay)->constants.shared==draw.constants.shared);
  SR_CHECK(std::memcmp(GetDraw(replay)->vertex_fetch.data(),draw.vertex_fetch.data(),sizeof(draw.vertex_fetch))==0);
  SR_CHECK(GetDraw(replay)->indices.normalized==draw.indices.normalized);
    SR_CHECK(draw.primitive == Primitive::kTriangleStrip);
    SR_CHECK(draw.primitive_restart);
  SR_CHECK_EQ(draw.vertex_fetch[0].type, 0x2c2259);
  SR_CHECK_EQ(draw.vertex_fetch[0].stride, 4);
  VertexFetchMeta metadata;
  std::memcpy(&metadata, draw.constants.shared.data() + 512, sizeof(metadata));
  SR_CHECK_EQ(metadata.type, 0x2c2259);
  // Reused guest addresses and later partial captures must invalidate/override
  // cached declaration bytes without changing an already-owned packet.
  batch.bytes[12+0x3b]=0x5f;
  RenderPacket changed;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,changed,error));
  SR_CHECK_EQ((*GetDraw(changed)).vertex_fetch[0].type,0x2c225fu);
  auto patch=uint32_t(batch.bytes.size());
  batch.bytes.insert(batch.bytes.end(),{0,0x2c,0x22,0x59});
  batch.ranges.push_back({0x3038,4,patch});++cmd.range_count;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,changed,error));
  SR_CHECK_EQ((*GetDraw(changed)).vertex_fetch[0].type,0x2c2259u);
  SR_CHECK_EQ(draw.vertex_fetch[0].type,0x2c2259u);
}
SR_TEST(same_capture_decodes_identically_for_both_backends) {
  WorkBatch batch;
  WorkCmd cmd;
  cmd.op = Op::kSwap;
  cmd.u[0] = 0x12345678;
  cmd.u64 = 999;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket d3d, vulkan;
  std::string error;
  SR_CHECK(DecodeRenderPacket(batch, cmd, mirror, d3d, error));
  SR_CHECK(DecodeRenderPacket(batch, cmd, mirror, vulkan, error));
  SR_CHECK_EQ(std::get<SwapPacket>(d3d).frontbuffer,
              std::get<SwapPacket>(vulkan).frontbuffer);
  SR_CHECK_EQ(std::get<SwapPacket>(vulkan).guest_swap, 999);
  cmd.op=Op::kDrawInline;
  cmd.u[0]=4;cmd.u[1]=0x1000;cmd.u[2]=3;cmd.u[3]=4;
  batch.bytes={1,2,3,4,5,6,7,8,9,10,11,12};
  batch.ranges={{0x1000,12,0}};cmd.range_count=1;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,d3d,error));
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,vulkan,error));
  auto& a=(*GetDraw(d3d));auto& b=(*GetDraw(vulkan));
  SR_CHECK(a.inline_data==b.inline_data);
  SR_CHECK(a.constants.vs==b.constants.vs);
  SR_CHECK(a.constants.ps==b.constants.ps);
  SR_CHECK(a.constants.shared==b.constants.shared);
  SR_CHECK(a.primitive==b.primitive);
  SR_CHECK_EQ(a.count,b.count);SR_CHECK_EQ(a.inline_stride,b.inline_stride);
}

SR_TEST(screen_shader_constants_preserve_pixel_centers_and_alpha_test) {
  DrawPacket draw;
  draw.has_device_state = true;
  draw.colors[0] = {1, false, {640, 360, 0, 0, 0, 0}};
  draw.viewport = {0, 0, 1280, 720, 0, 1};
  draw.registers[0x206] = 0;       // screen coordinates
  draw.registers[0x202] = 0x8 | 6; // alpha GEQUAL
  draw.registers[0x10e] = std::bit_cast<uint32_t>(0.5f);
  FinalizeDrawConstants(draw);
  auto f = [&](uint32_t word) {
    float v;
    std::memcpy(&v, draw.constants.shared.data() + word * 4, 4);
    return v;
  };
  SR_CHECK(f(73) == 1.0f / 640);
  SR_CHECK(f(74) == -1.0f / 360);
  SR_CHECK(f(108) == 2.0f / 640);
  SR_CHECK(f(109) == -2.0f / 360);
  SR_CHECK(f(110) == -1.0f);
  SR_CHECK(f(111) == 1.0f);
  SR_CHECK(f(75) == 0.5f);
  SR_CHECK(f(113) == 1.0f);
  uint32_t spec;
  std::memcpy(&spec, draw.constants.shared.data() + 112 * 4, 4);
  SR_CHECK_EQ(spec, 2u);
}

SR_TEST(indexed_quads_require_owned_expansion) {
  WorkBatch batch;
  WorkCmd cmd;
  cmd.op = Op::kDrawIndexed;
  cmd.u[0] = 13;
  cmd.u[3] = 4;
  cmd.has_index = true;
  RenderPacket out = SwapPacket{99, 7};
  std::string error;
  superman_returns::native::Pm4Mirror mirror;
  SR_CHECK(!DecodeRenderPacket(batch, cmd, mirror, out, error));
  SR_CHECK(std::holds_alternative<SwapPacket>(out));
  SR_CHECK_EQ(std::get<SwapPacket>(out).frontbuffer, 99u);
}

SR_TEST(resolve_packet_keeps_mip_slice_and_owned_regions) {
  WorkBatch batch;
  batch.bytes={0,0,0,1,0,0,0,2,0,0,0,9,0,0,0,10};
  batch.ranges={{0x1000,16,0}};
  WorkCmd cmd;cmd.op=Op::kResolve;cmd.u[1]=0x1000;
  cmd.u[6]=2;cmd.u[7]=5;cmd.range_count=1;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket out;std::string error;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,out,error));
  batch.Clear();
  const auto& resolve=std::get<ResolvePacket>(out);
  SR_CHECK_EQ(resolve.level,2u);SR_CHECK_EQ(resolve.slice,5u);
  SR_CHECK_EQ(resolve.source.left,1);SR_CHECK_EQ(resolve.source.bottom,10);
}
SR_TEST(captured_replay_rejects_unreadable_indirect_and_failed_capture) {
  WorkBatch batch;batch.bytes={0xc0,0x01,0x3f,0,0,0,2,0,0,0,0,4};WorkCmd cmd;cmd.op=Op::kRing;cmd.ring_bytes=12;
  superman_returns::native::Pm4Mirror mirror;RenderPacket packet;std::string error;
  SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,packet,error));SR_CHECK(!error.empty());
  cmd.pm4_capture_ok=false;SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,packet,error));SR_CHECK(error.find("capture")!=std::string::npos);
}
SR_TEST(captured_replay_checks_primary_bounds_and_decodes_without_live_memory) {
  WorkBatch batch;WorkCmd cmd;cmd.op=Op::kRing;cmd.pass=7;superman_returns::native::Pm4Mirror mirror;RenderPacket packet;std::string error;
  SR_CHECK(ReplayCapturedRenderPacket(batch,cmd,mirror,packet,error));SR_CHECK_EQ(std::get<PassPacket>(packet).pass,7);
  cmd.ring_offset=UINT32_MAX;cmd.ring_bytes=8;SR_CHECK(!ReplayCapturedRenderPacket(batch,cmd,mirror,packet,error));
}
SR_TEST(shader_roles_follow_owned_container_stage_when_device_slots_are_reversed) {
  WorkBatch batch;WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;
  auto vs=std::make_shared<ShaderCapture>(),ps=std::make_shared<ShaderCapture>();vs->vertex=true;vs->hash=17;ps->hash=29;
  cmd.vertex_shader=ps;cmd.pixel_shader=vs;
  superman_returns::native::Pm4Mirror mirror;RenderPacket packet;std::string error;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,packet,error));const auto& draw=(*GetDraw(packet));
  SR_CHECK(draw.vertex_shader==vs);SR_CHECK(draw.pixel_shader==ps);
}
SR_TEST(contiguous_capture_fast_path_rejects_partial_overlays) {
  WorkBatch batch;
  batch.bytes={1,2,3,4,9,8};
  batch.ranges={{0x1000,4,0},{0x1001,2,4}};
  WorkCmd cmd;cmd.range_count=2;
  CapturedMemory memory;std::string error;
  SR_CHECK(CapturedMemory::Capture(batch,cmd,memory,error));
  SR_CHECK(memory.ReadUnambiguous(0x1000,4).empty());
  SR_CHECK_EQ(memory.ReadUnambiguous(0x1001,2)[0],9);
  SR_CHECK_EQ(memory.Read(0x1000,4)[1],2);
}
SR_TEST(device_bulk_decode_preserves_latest_word_overlays_and_pm4_precedence) {
  namespace profile=superman_returns::native::profile;
  WorkBatch batch;batch.bytes.resize(0x4000,0);batch.ranges={{0x10000,0x4000,0}};
  auto store=[&](uint32_t offset,uint32_t value) {
    for(uint32_t byte=0;byte<4;++byte) batch.bytes[offset+byte]=uint8_t(value>>(24-byte*8));
  };
  for(auto range:profile::kRegisterShadow)
    for(uint32_t i=0;i<range.count;++i) store(range.offset+i*4,range.first+i);
  for(uint32_t i=0;i<192;++i) store(profile::kDevice.fetch_constants+i*4,i+1);
  const auto shadow=profile::kRegisterShadow[0];
  batch.bytes.insert(batch.bytes.end(),{0x12,0x34,0x56,0x78,0x87,0x65,0x43,0x21});
  batch.ranges.push_back({0x10000+shadow.offset+4,4,0x4000});
  batch.ranges.push_back({0x10000+profile::kDevice.fetch_constants+4,4,0x4004});
  WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;cmd.device=0x10000;cmd.range_count=3;
  superman_returns::native::Pm4Mirror mirror;
  const uint8_t fetch_write[]={0,0,0x48,0,0xaa,0xbb,0xcc,0xdd};
  mirror.ScanCopy(nullptr,fetch_write,sizeof(fetch_write));
  RenderPacket packet;std::string error;
  SR_CHECK(DecodeRenderPacket(batch,cmd,mirror,packet,error));
  const auto& draw=*GetDraw(packet);
  SR_CHECK_EQ(draw.registers[0],0x2000u);
  SR_CHECK_EQ(draw.registers[1],0x12345678u);
  SR_CHECK_EQ(draw.registers[2],0x2002u);
  SR_CHECK_EQ(draw.texture_fetch[0][0],0xaabbccddu);
  SR_CHECK_EQ(draw.texture_fetch[0][1],0x87654321u);
  SR_CHECK_EQ(draw.texture_fetch[31][5],192u);
}
SR_TEST(bulk_capture_accepts_only_byte_identical_partial_overlays) {
  WorkBatch batch;
  // CaptureDevice appends a duplicate vertex-fetch suffix after the full bank.
  batch.bytes.resize(768);
  for(size_t i=0;i<768;++i) batch.bytes[i]=uint8_t(i*17);
  const std::vector<uint8_t> suffix(batch.bytes.begin()+640,batch.bytes.end());
  batch.bytes.insert(batch.bytes.end(),suffix.begin(),suffix.end());
  batch.bytes.insert(batch.bytes.end(),{batch.bytes[16],batch.bytes[17]});
  batch.ranges={{0x1000,768,0},{0x1280,128,768},{0x1010,2,896}};
  WorkCmd cmd;cmd.range_count=3;
  CapturedMemory memory;std::string error;
  SR_CHECK(CapturedMemory::Capture(batch,cmd,memory,error));
  const auto full=memory.ReadUnambiguous(0x1000,768);
  SR_CHECK_EQ(full.size(),768u);
  if(full.size()==768) for(size_t i=0;i<768;++i) SR_CHECK_EQ(full[i],uint8_t(i*17));
  // A single changed byte retains the scalar overlay path.
  batch.bytes[897]^=1;
  SR_CHECK(CapturedMemory::Capture(batch,cmd,memory,error));
  SR_CHECK(memory.ReadUnambiguous(0x1000,768).empty());
  // A newer complete capture supersedes all earlier partial overlays.
  batch.ranges.push_back({0x1000,768,0});cmd.range_count=4;
  SR_CHECK(CapturedMemory::Capture(batch,cmd,memory,error));
  SR_CHECK_EQ(memory.ReadUnambiguous(0x1000,768).size(),768u);
  // Compare only the queried intersection of a suffix extending past the bank.
  std::vector<uint8_t> clipped(batch.bytes.begin(),batch.bytes.begin()+768);
  clipped.insert(clipped.end(),batch.bytes.begin()+760,batch.bytes.begin()+768);
  clipped.resize(784,0xa5);
  std::array<CaptureRange,2> ranges{{{0x1000,768,0},{0x12f8,16,768}}};
  SR_CHECK_EQ(FindUnambiguousCapture(ranges,clipped,0x1000,768).size(),768u);
  clipped[775]^=1;
  SR_CHECK(FindUnambiguousCapture(ranges,clipped,0x1000,768).empty());
  ranges[1].offset=UINT32_MAX;
  SR_CHECK(FindUnambiguousCapture(ranges,clipped,0x1000,768).empty());
  SR_CHECK(FindUnambiguousCapture(ranges,clipped,0xfffffffeu,4).empty());
}
SR_TEST(replayed_draw_keeps_latest_capture_and_pm4_constants_after_batch_reuse) {
  WorkBatch batch;
  // Primary PM4 writes VS c0.x; overlapping captures model a later guest write.
  batch.bytes = {0, 0, 0x40, 0, 0x3f, 0x80, 0, 0, 1, 2, 3, 4, 9, 8};
  batch.ranges = {{0x1000, 4, 8}, {0x1001, 2, 12}};
  WorkCmd cmd;
  cmd.op = Op::kDraw;
  cmd.u[0] = 4;
  cmd.ring_bytes = 8;
  cmd.range_count = 2;
  superman_returns::native::Pm4Mirror mirror;
  RenderPacket packet;
  std::string error;
  SR_CHECK(ReplayCapturedRenderPacket(batch, cmd, mirror, packet, error));
  batch.Clear();
  batch.bytes.resize(1024, 0xff);
  mirror = superman_returns::native::Pm4Mirror{};
  const auto& draw = (*GetDraw(packet));
  SR_CHECK_EQ(draw.constants.vs[0], 0x3f800000u);
  SR_CHECK_EQ(draw.memory.Read(0x1001, 2)[0], 9);
  SR_CHECK_EQ(draw.memory.Read(0x1000, 4)[1], 2);
}
