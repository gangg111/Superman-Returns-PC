#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <mutex>
#include "pm4_mirror.h"
#include "texture_content.h"
#include "../graphics/guest/texture_capture.h"

namespace superman_returns::native {

struct CapturedTextureEntry {
  std::shared_ptr<const graphics::guest::TextureCapture> snapshot;
  uint64_t content_hash=0, checked_frame=~0ull;
  uint32_t watch_seq=0;
  CleanTextureWatchScan watch_scan;
  uint64_t next_check_frame=0;
  uint32_t stable_checks=0;
  uint64_t failed_frame=~0ull;
  std::string failure;
};

struct RingConstants {
  bool pixel;
  uint32_t start, count, ring_ptr;
};

struct NativeFrontend {
  NativeFrontend()=default;
  NativeFrontend(const NativeFrontend&)=delete;
  NativeFrontend& operator=(const NativeFrontend&)=delete;
  NativeFrontend(NativeFrontend&&)=delete;
  NativeFrontend& operator=(NativeFrontend&&)=delete;
  Pm4Mirror mirror_;
  Pm4Mirror capture_mirror_;
  uint64_t capture_serial_ = 0;
  bool capture_tiling_active_ = false;
  std::map<std::array<uint32_t,6>, CapturedTextureEntry> captured_textures_;
  struct TextureSlotLookup {
    std::array<uint32_t,6> fetch{};
    CapturedTextureEntry* entry=nullptr;
  };
  std::array<TextureSlotLookup,32> texture_slot_lookups_{};
  CapturedTextureEntry& CapturedTexture(uint32_t slot,const std::array<uint32_t,6>& fetch) {
    auto& lookup=texture_slot_lookups_[slot];
    if(!lookup.entry || lookup.fetch!=fetch) {
      lookup.entry=&captured_textures_[fetch];lookup.fetch=fetch;
    }
    // Cache only the map lookup: callers still validate writes/content on every
    // use. std::map keeps node addresses stable while new entries are inserted.
    return *lookup.entry;
  }
  void InvalidateTextureSlotLookups() {
    for(auto& lookup:texture_slot_lookups_) lookup.entry=nullptr;
  }
  std::vector<uint32_t> mirror_snapshot_;
  uint32_t ring_last_ = 0;
  uint64_t ring_resyncs_ = 0;
  std::vector<RingConstants> pending_ring_constants_;
  // Guards pending_ring_constants_ only: the game thread notes and flushes them
  // on every draw, so it must not wait for mutex_, which the worker holds for a
  // whole batch.
  std::mutex ring_constants_mutex_;
  // Offset of the last float-bank capture in the current batch (CaptureDevice).
  uint32_t last_constants_offset_ = ~0u;
  uint64_t swap_number_ = 0;
  uint64_t profiled_texture_writes_ = 0;
  uint32_t pass_draw_index_[32] = {};
  uint32_t trace_draws_ = 0;
};

} // namespace superman_returns::native
