#pragma once

#include <rex/ui/overlay/debug_overlay.h>

// FPS of the guest's own presents, averaged over ~0.5 s windows.
rex::ui::FrameStats SampleGuestFrameStats();

// Guest presents since start (one per game frame; the input replay counts by it).
uint64_t GuestFrameCount();
