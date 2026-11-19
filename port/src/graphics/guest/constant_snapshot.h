#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
namespace superman_returns::graphics::guest {
struct ConstantSnapshot {
  // Replay storage: the banks are the replay's shared ones (DrawPacket::vs_bank / ps_bank) and
  // shared[512,4096) is never written or read (the recorders take the vertex fetch table from
  // DrawPacket::vertex_fetch), so only the prefix, with partial writes and padding, starts at zero.
  // Other callers retain fully initialized default storage.
  struct ReplayStorageTag {};
  ConstantSnapshot() : vs{},ps{},shared{} {}
  explicit ConstantSnapshot(ReplayStorageTag) {std::fill_n(shared.begin(),512,uint8_t{0});}
  std::array<uint32_t, 1024> vs, ps;
  std::array<uint8_t, 4096> shared;
};
bool CaptureFloatConstants(std::span<const uint32_t>, bool guest_big_endian,
                           std::array<uint32_t, 1024> &, std::string &);
} // namespace superman_returns::graphics::guest
