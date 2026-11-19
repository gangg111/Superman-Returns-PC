#pragma once
#include <algorithm>
#include <cstdint>

namespace superman_returns::native {
// Page writes may belong to a neighboring allocation. Small buffers confirm
// their contents before invalidating; the frame check also covers virtual aliases.
// max_backoff_log2 > 0 (a watched buffer: page writes are seen without it): a buffer found
// unchanged is checked again after 2, 4 .. 2^max_backoff_log2 frames instead of every frame, like
// the textures' backoff; a change or a page write checks it again the next frame. Hashing every
// small buffer once a frame was ~4% of the game's render thread in the city (Fold7, 2026-11-19).
struct BufferContent {
  uint64_t value=0,frame=~uint64_t(0),next_check=0;
  uint32_t stable=0;
  bool valid=false;
  template<class Hash> bool Refresh(uint64_t current_frame,bool page_written,Hash hash,bool every_use=false,
                                    uint32_t max_backoff_log2=0) {
    if(valid && !page_written && !every_use && (frame==current_frame || current_frame<next_check)) return false;
    const uint64_t next=hash();
    const bool changed=valid && next!=value;
    stable=(!valid || changed || page_written) ? 0 : std::min(stable+1,max_backoff_log2);
    next_check=current_frame+(uint64_t(1)<<stable);
    value=next;frame=current_frame;valid=true;
    return changed;
  }
};
}
