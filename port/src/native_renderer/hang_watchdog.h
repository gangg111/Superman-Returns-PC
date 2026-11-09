// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/hang_watchdog.h (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
#pragma once

namespace superman_returns::native {

// Called once per guest swap; starts the watchdog thread on first use.
void HangWatchdogBeat();

// Called by the render worker thread at start: sr_native_sample_worker_at_s
// samples it like the swap thread.
void RegisterWorkerThreadForSampling();

// Keeps the calling thread on the CPU's performance cores (hybrid CPUs, e.g. i9-12900K:
// 8 P-cores + 8 E-cores) and out of power throttling (sr_pin_performance_cores).
void PreferPerformanceCores(const char* name);

}  // namespace superman_returns::native
