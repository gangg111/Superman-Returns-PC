// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/hang_watchdog.cpp (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Hang watchdog: when guest swaps stop for sr_native_hang_watchdog_s seconds, logs
// the native call stacks of every process thread once (dbghelp, superman_returns.pdb).
// Diagnoses freezes without an attached debugger.
#include "hang_watchdog.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <deque>
#include <map>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(sr_native_hang_watchdog_s, 6, "Superman Returns Native",
                     "Log all thread stacks when no swap happens for this many seconds (0 = off)");
REXCVAR_DEFINE_INT32(sr_native_sample_swap_thread_at_s, 0, "Superman Returns Native",
                     "Debug: at this many seconds after the first swap, sample the game thread's "
                     "stack for 10 s and log the most frequent stacks (0 = off)");
REXCVAR_DEFINE_BOOL(sr_pin_performance_cores, true, "Superman Returns Native",
                    "Keep the game thread and the render worker on the CPU's performance cores (hybrid "
                    "CPUs): on an efficiency core the worker's frame took ~40% longer");
REXCVAR_DEFINE_BOOL(sr_pin_split_cores, true, "Superman Returns Native",
                    "Give the game thread and the render worker separate halves of the performance cores, "
                    "so they never share one physical core through hyper-threading");
REXCVAR_DEFINE_INT32(sr_native_hitch_sampler_ms, 0, "Superman Returns Native",
                     "Debug: sample the game thread's stack every 2 ms and, for every game frame longer "
                     "than this many ms, log the stacks seen during that frame (0 = off)");
REXCVAR_DEFINE_INT32(sr_native_sample_worker_at_s, 0, "Superman Returns Native",
                     "Debug: as sr_native_sample_swap_thread_at_s, for the render worker thread, "
                     "every 2 ms for 10 s, logging every distinct stack (0 = off)");

// generated/default/superman_returns_init.cpp, terminated by { 0, nullptr }.
extern PPCFuncMapping PPCFuncMappings[];

namespace superman_returns::native {
namespace {

std::atomic<int64_t> g_last_beat_ms{0};
std::atomic<uint32_t> g_beat_thread{0};
std::atomic<uint32_t> g_worker_thread{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Recompiled guest code has no symbols: name an executable address after the
// recompiled function starting closest below it (PPCFuncMappings, generated).
std::string GuestFunctionName(uint64_t address) {
  static const std::vector<std::pair<uint64_t, uint32_t>> starts = [] {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
      v.emplace_back(reinterpret_cast<uint64_t>(m->host), uint32_t(m->guest));
    std::sort(v.begin(), v.end());
    return v;
  }();
  auto it = std::upper_bound(starts.begin(), starts.end(), std::pair{address, ~0u});
  if (it == starts.begin() || address - std::prev(it)->first > (1u << 20)) return {};
  --it;
  return fmt::format("sub_{:08X}+0x{:X}", it->second, address - it->first);
}

// Copy of the walked thread's stack. While a thread is suspended nothing may
// allocate or take a lock it could hold (heap, dbghelp): only its context and
// stack are copied, and the walk runs on the copy after it resumes.
alignas(16) uint8_t g_stack_copy[256 * 1024];
uint64_t g_stack_copy_base = 0, g_stack_copy_size = 0;

BOOL CALLBACK ReadStackCopy(HANDLE process, DWORD64 address, PVOID buffer, DWORD size,
                            LPDWORD read) {
  if (address >= g_stack_copy_base && address + size <= g_stack_copy_base + g_stack_copy_size) {
    std::memcpy(buffer, g_stack_copy + (address - g_stack_copy_base), size);
    *read = size;
    return TRUE;
  }
  SIZE_T done = 0;
  BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &done);
  *read = DWORD(done);
  return ok;
}

// Returns the thread's call stack, one "\n    frame" per frame.
std::string WalkThread(HANDLE process, DWORD tid, int max_frames) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, tid);
  if (!thread) return {};
  if (SuspendThread(thread) == DWORD(-1)) {
    CloseHandle(thread);
    return {};
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  bool have_context = GetThreadContext(thread, &ctx);
  g_stack_copy_base = g_stack_copy_size = 0;
  MEMORY_BASIC_INFORMATION region{};
  if (have_context && VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rsp), &region, sizeof(region))) {
    uint64_t end = uint64_t(region.BaseAddress) + region.RegionSize;
    uint64_t size = std::min<uint64_t>(sizeof(g_stack_copy), end - ctx.Rsp);
    SIZE_T done = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(ctx.Rsp), g_stack_copy, size, &done)) {
      g_stack_copy_base = ctx.Rsp;
      g_stack_copy_size = done;
    }
  }
  ResumeThread(thread);
  std::string out;
  if (have_context) {
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < max_frames; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, ReadStackCopy,
                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
          !frame.AddrPC.Offset) {
        break;
      }
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
      auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 255;
      DWORD64 disp = 0;
      if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym)) {
        out += fmt::format("\n    {}+0x{:X}", sym->Name, disp);
      } else if (auto name = GuestFunctionName(frame.AddrPC.Offset); !name.empty()) {
        // The executable offset resolves renderer frames against a linker map.
        static const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        out += fmt::format("\n    {} [exe+0x{:X}]", name, frame.AddrPC.Offset - exe);
      } else {
        out += fmt::format("\n    0x{:X}", frame.AddrPC.Offset);
      }
    }
  }
  CloseHandle(thread);
  return out;
}

void DumpThread(HANDLE process, DWORD tid, bool is_render) {
  REXLOG_WARN("native watchdog: thread {}{}:{}", tid, is_render ? " (swap thread)" : "",
              WalkThread(process, tid, 24));
}

// sr_native_sample_swap_thread_at_s: once, at that many seconds after the
// first swap, sample the swap (game) thread's stack every 20 ms for 10 s and
// log the most frequent stacks. Shows where the game thread spends its frame,
// e.g. the wait of the game's own frame limiter.
// `all` logs every distinct stack, 32 frames deep, instead of the 40 most
// frequent ones: the worker's renderer frames are exe offsets, so self and
// inclusive time per function are summed offline against the linker map
// (SR_LINK_MAP).
std::string WalkThreadUnwind(DWORD tid, int max_frames);

void SampleThread(const char* label, DWORD tid, uint32_t samples, uint32_t interval_ms, bool all) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  std::map<std::string, uint32_t> stacks;
  for (uint32_t i = 0; i < samples; ++i) {
    ++stacks[WalkThreadUnwind(tid, all ? 32 : 12)];
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  SymCleanup(process);
  std::vector<std::pair<uint32_t, std::string>> sorted;
  for (auto& [stack, count] : stacks) sorted.emplace_back(count, stack);
  std::sort(sorted.rbegin(), sorted.rend());
  for (size_t i = 0; i < sorted.size() && (all || i < 40); ++i) {
    REXLOG_INFO("native {} sample: {}/{} samples:{}", label, sorted[i].first, samples,
                sorted[i].second);
  }
  rex::FlushLogging();
}

void SampleSwapThread() { SampleThread("swap thread", g_beat_thread.load(), 2500, 4, true); }

void DumpAllThreads() {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  DWORD self = GetCurrentThreadId();
  DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    DumpThread(process, te.th32ThreadID, te.th32ThreadID == g_beat_thread.load());
  }
  CloseHandle(snap);
  SymCleanup(process);
  rex::FlushLogging();
}

void WatchdogMain() {
  bool reported = false;
  const int64_t started_ms = NowMs();
  bool sampled = false, worker_sampled = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (int32_t at_s = REXCVAR_GET(sr_native_sample_swap_thread_at_s);
        !sampled && at_s > 0 && NowMs() - started_ms >= int64_t(at_s) * 1000) {
      sampled = true;
      SampleSwapThread();
    }
    if (int32_t at_s = REXCVAR_GET(sr_native_sample_worker_at_s);
        !worker_sampled && at_s > 0 && g_worker_thread.load() &&
        NowMs() - started_ms >= int64_t(at_s) * 1000) {
      worker_sampled = true;
      SampleThread("worker", g_worker_thread.load(), 5000, 2, true);
    }
    int32_t limit_s = REXCVAR_GET(sr_native_hang_watchdog_s);
    int64_t last = g_last_beat_ms.load();
    if (limit_s <= 0 || !last) continue;
    int64_t idle = NowMs() - last;
    if (idle < int64_t(limit_s) * 1000) {
      reported = false;
      continue;
    }
    if (!reported) {
      reported = true;
      REXLOG_WARN("native watchdog: no swap for {} ms, dumping thread stacks", idle);
      DumpAllThreads();
    }
  }
}

// The thread's return addresses by RtlVirtualUnwind (.pdata of every loaded
// module, the driver's too) while it is suspended: nothing here allocates or
// locks. Then named outside the suspension: exe frames as in WalkThread (guest
// function or exe offset for the linker map), others as module+offset.
std::string WalkThreadUnwind(DWORD tid, int max_frames) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!thread) return {};
  uint64_t pcs[48];
  int n = 0;
  if (SuspendThread(thread) != DWORD(-1)) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &ctx)) {
      for (; n < max_frames && n < 48 && ctx.Rip; ++n) {
        pcs[n] = ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!fe) {  // leaf function: the return address is on top of the stack
          ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
          ctx.Rsp += 8;
        } else {
          PVOID handler_data = nullptr;
          DWORD64 frame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &frame, nullptr);
        }
      }
    }
    ResumeThread(thread);
  }
  CloseHandle(thread);
  static const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
  std::string out;
  for (int i = 0; i < n; ++i) {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(pcs[i]), &mod);
    if (reinterpret_cast<uint64_t>(mod) == exe) {
      auto name = GuestFunctionName(pcs[i]);
      out += fmt::format("\n    {} [exe+0x{:X}]", name.empty() ? "exe" : name, pcs[i] - exe);
    } else if (mod) {
      wchar_t path[MAX_PATH];
      const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
      std::wstring w(path, len);
      const auto slash = w.find_last_of(L"\\/");
      std::string base;
      for (wchar_t c : w.substr(slash == std::wstring::npos ? 0 : slash + 1)) base += char(c);
      out += fmt::format("\n    {}+0x{:X}", base, pcs[i] - reinterpret_cast<uint64_t>(mod));
    } else {
      out += fmt::format("\n    0x{:X}", pcs[i]);
    }
  }
  return out;
}

// sr_native_hitch_sampler_ms: the game thread's stack every 2 ms into a ring
// of the last second; a long game frame (HangWatchdogBeat) asks for the
// samples of its own time window, logged here (counts per distinct stack).
std::atomic<int64_t> g_hitch_from{0}, g_hitch_to{0};
std::atomic<int32_t> g_hitch_ms{0};

void HitchSamplerMain() {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  // Game thread and render worker, sampled together (the game thread mostly
  // waits for the worker when the worker falls behind).
  std::deque<std::pair<int64_t, std::string>> ring, worker_ring;
  auto report = [](const char* label, const std::deque<std::pair<int64_t, std::string>>& r, int64_t from,
                   int64_t to, size_t top) {
    std::map<std::string, uint32_t> stacks;
    uint32_t n = 0;
    for (const auto& [t, st] : r) {
      if (t < from || t > to) continue;
      ++stacks[st];
      ++n;
    }
    std::vector<std::pair<uint32_t, std::string>> sorted;
    for (auto& [st, c] : stacks) sorted.emplace_back(c, st);
    std::sort(sorted.rbegin(), sorted.rend());
    REXLOG_INFO("native hitch {}: {} samples", label, n);
    for (size_t i = 0; i < sorted.size() && i < top; ++i)
      REXLOG_INFO("native hitch {} stack: {}/{}:{}", label, sorted[i].first, n, sorted[i].second);
  };
  int64_t modules_ms = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Driver and D3D12 modules load after start: without them the walk stops
    // at the first driver frame instead of reaching the renderer's caller.
    if (NowMs() - modules_ms > 1000) {
      modules_ms = NowMs();
      SymRefreshModuleList(process);
    }
    const DWORD tid = g_beat_thread.load(), worker = g_worker_thread.load();
    if (!tid) continue;
    const int64_t now = NowMs();
    ring.emplace_back(now, WalkThreadUnwind(tid, 24));
    if (worker) worker_ring.emplace_back(now, WalkThreadUnwind(worker, 28));
    while (!ring.empty() && ring.front().first < now - 1000) ring.pop_front();
    while (!worker_ring.empty() && worker_ring.front().first < now - 1000) worker_ring.pop_front();
    const int64_t to = g_hitch_to.exchange(0);
    if (!to) continue;
    const int64_t from = g_hitch_from.load();
    REXLOG_INFO("native hitch: game frame {} ms", g_hitch_ms.load());
    report("game", ring, from, to, 10);
    report("worker", worker_ring, from, to, 12);
  }
}

}  // namespace

void HangWatchdogBeat() {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(WatchdogMain).detach(); });
  const int64_t now = NowMs(), prev = g_last_beat_ms.load();
  if (const int32_t limit = REXCVAR_GET(sr_native_hitch_sampler_ms); limit > 0) {
    static std::once_flag sampler;
    std::call_once(sampler, [] { std::thread(HitchSamplerMain).detach(); });
    if (prev && now - prev > limit && GetCurrentThreadId() == g_beat_thread.load()) {
      g_hitch_ms.store(int32_t(now - prev));
      g_hitch_from.store(prev);
      g_hitch_to.store(now);
    }
  }
  if (thread_local bool preferred = false; !preferred) {
    preferred = true;
    PreferPerformanceCores("game");
  }
  g_beat_thread.store(GetCurrentThreadId());
  g_last_beat_ms.store(now);
}

void RegisterWorkerThreadForSampling() {
  g_worker_thread.store(GetCurrentThreadId());
  PreferPerformanceCores("render worker");
}

void PreferPerformanceCores(const char* name) {
  if (!REXCVAR_GET(sr_pin_performance_cores)) return;
  // The CPU sets of the highest efficiency class (the P-cores; on a CPU with
  // one class this is every core and changes nothing).
  ULONG length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
  std::vector<uint8_t> buffer(length);
  if (!length || !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length,
                                             &length, GetCurrentProcess(), 0)) {
    return;
  }
  struct Set {
    BYTE cls;
    BYTE core;
    ULONG id;
  };
  std::vector<Set> sets;
  BYTE best = 0;
  for (ULONG offset = 0; offset < length;) {
    const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (info->Type == CpuSetInformation) {
      sets.push_back({info->CpuSet.EfficiencyClass, info->CpuSet.CoreIndex, info->CpuSet.Id});
      best = std::max(best, info->CpuSet.EfficiencyClass);
    }
    offset += info->Size;
  }
  // The game thread and the render worker each take half of the P-cores'
  // physical cores, so they never share one core through hyper-threading
  // (sharing one made the worker's frame time jump between ~10 and ~15 ms
  // from run to run).
  std::vector<BYTE> cores;
  for (const auto& s : sets)
    if (s.cls == best && std::find(cores.begin(), cores.end(), s.core) == cores.end()) cores.push_back(s.core);
  std::sort(cores.begin(), cores.end());
  const bool game = std::strcmp(name, "game") == 0, worker = std::strcmp(name, "render worker") == 0;
  const size_t half = cores.size() / 2;
  std::vector<ULONG> ids;
  for (const auto& s : sets) {
    if (s.cls != best) continue;
    const size_t at = size_t(std::find(cores.begin(), cores.end(), s.core) - cores.begin());
    if (REXCVAR_GET(sr_pin_split_cores) && half >= 2 && ((game && at >= half) || (worker && at < half))) continue;
    ids.push_back(s.id);
  }
  const bool pinned = !ids.empty() && ids.size() < sets.size() &&
                      SetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), ULONG(ids.size()));
  // No EcoQoS: Windows may otherwise slow a thread it thinks is in the background.
  THREAD_POWER_THROTTLING_STATE throttling{};
  throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  throttling.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
  REXLOG_INFO("cpu: {} thread on {} of {} CPU sets (efficiency class {}, {} physical cores){}", name, ids.size(),
              sets.size(), best, cores.size(), pinned ? "" : ", not pinned");
}

}  // namespace superman_returns::native
