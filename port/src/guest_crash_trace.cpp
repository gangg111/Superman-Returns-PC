// Debug: the guest's call stack when the runtime aborts.
//
// REX_FATAL (e.g. "Call to invalid or unregistered function at guest address 0x20")
// logs one line and calls std::abort(), with nothing about where the guest was.
// Both the runtime DLL and this exe use the shared UCRT, so a SIGABRT handler set
// here also runs for the runtime's abort, on the aborting thread. It logs that
// thread's PPC context (last indirect target, LR, r1, r3..r5) and walks the guest
// stack: a frame's back chain at [sp], the caller's saved LR at back chain - 8
// (prolog stw r12,-8(r1) + stwu). Every read is checked with VirtualQuery: a walk
// past the stack's end must not fault inside the handler.
#include <csignal>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

#include "generated/default/superman_returns_init.h"

namespace superman_returns {
namespace {

uint8_t* g_membase = nullptr;

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const auto end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<const uint8_t*>(p) + n <= end;
}

bool GuestU32(uint32_t address, uint32_t& value) {
  if (!g_membase || address < 0x1000) return false;
  const uint8_t* p = g_membase + address;
  if (!Readable(p, 4)) return false;
  uint32_t v;
  std::memcpy(&v, p, 4);
  value = _byteswap_ulong(v);
  return true;
}

void OnAbort(int);

// Unhandled exceptions (an access violation in recompiled code): the same trace,
// then the filter that was there before (the runtime's or Windows' report).
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  const auto* rec = info ? info->ExceptionRecord : nullptr;
  REXLOG_CRITICAL("crash trace: unhandled exception {:08X} at host {} (exe offset {:X}), address {:X}",
                  rec ? uint32_t(rec->ExceptionCode) : 0u, rec ? rec->ExceptionAddress : nullptr,
                  rec ? uintptr_t(rec->ExceptionAddress) - uintptr_t(GetModuleHandleW(nullptr)) : 0u,
                  rec && rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0u);
  OnAbort(0);
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) {
  auto* ts = rex::runtime::ThreadState::Get();
  PPCContext* ctx = ts ? ts->context() : nullptr;
  if (!ctx) {
    REXLOG_CRITICAL("crash trace: abort on a thread without a guest context");
  } else {
    REXLOG_CRITICAL("crash trace: thread {} target {:08X} lr {:08X} r1 {:08X} r3 {:08X} r4 {:08X} r5 {:08X}",
                    rex::runtime::ThreadState::GetThreadID(), ctx->last_indirect_target, uint32_t(ctx->lr),
                    ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32);
    // sub_82697398 releases a container's entries (count at +0, 8-byte entries at
    // +4, the value at entry + 4; r31 the container, r30 the entry's offset): the
    // entry it died on, its key and value words and the bytes they point at (a
    // member's name tells who kept the freed object).
    if (uint32_t(ctx->lr) == 0x82697440) {
      const uint32_t box = ctx->r31.u32, at = ctx->r30.u32;
      uint32_t count = 0, entries = 0, key = 0, value = 0;
      GuestU32(box, count);
      GuestU32(box + 4, entries);
      GuestU32(entries + at, key);
      GuestU32(entries + at + 4, value);
      REXLOG_CRITICAL("crash trace: container {:08X} count {} entries {:08X} offset {:X}: key {:08X} value {:08X}",
                      box, count, entries, at, key, value);
      for (uint32_t p : {key, value}) {
        char text[65] = {};
        uint32_t words[8] = {};
        for (int i = 0; i < 8; ++i) GuestU32(p + 4 * i, words[i]);
        if (g_membase && p >= 0x1000 && Readable(g_membase + p, 64))
          for (int i = 0; i < 64; ++i) {
            const char c = char(g_membase[p + i]);
            text[i] = c >= 32 && c < 127 ? c : '.';
          }
        REXLOG_CRITICAL("crash trace:   {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} |{}|", p,
                        words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], text);
      }
      // every entry's key text (the container's members), as far as readable
      for (uint32_t i = 0; i < count && i < 64; ++i) {
        uint32_t k = 0, v = 0;
        if (!GuestU32(entries + 8 * i, k) || !GuestU32(entries + 8 * i + 4, v)) break;
        char text[33] = {};
        if (g_membase && k >= 0x1000 && Readable(g_membase + k, 32))
          for (int j = 0; j < 32; ++j) {
            const char c = char(g_membase[k + j]);
            text[j] = c >= 32 && c < 127 ? c : '.';
          }
        REXLOG_CRITICAL("crash trace:   entry {:2} key {:08X} value {:08X} |{}|", i, k, v, text);
      }
    }
    uint32_t sp = ctx->r1.u32;
    for (int depth = 0; depth < 32; ++depth) {
      uint32_t back = 0, ret = 0;
      if (!GuestU32(sp, back) || back <= sp || back - sp > 0x100000) break;
      if (!GuestU32(back - 8, ret)) break;
      REXLOG_CRITICAL("crash trace: #{:02} frame {:08X} return {:08X}", depth, back, ret);
      sp = back;
    }
  }
  if (auto logger = rex::GetLogger()) logger->flush();
}

// The printable text at a guest address (up to n bytes), dots for the rest.
std::string GuestText(uint32_t p, int n) {
  std::string text;
  if (!g_membase || p < 0x1000 || !Readable(g_membase + p, size_t(n))) return text;
  for (int i = 0; i < n; ++i) {
    const char c = char(g_membase[p + i]);
    text += c >= 32 && c < 127 ? c : '.';
  }
  return text;
}

}  // namespace
}  // namespace superman_returns

// sub_82697398 (Apt: an object's member table released, each value's vtable[1]
// called): before it runs, a value whose vtable[1] is not game code is a freed
// object. Then every key and value of that table is logged while they are still
// there (the crash trace's own walk sees them cleared): the event's Type, for one.
REX_HOOK_RAW(sub_82697398) {
  using namespace superman_returns;
  static int logged = 0;
  const uint32_t box = ctx.r3.u32;
  uint32_t count = 0, entries = 0;
  if (logged < 10 && GuestU32(box, count) && GuestU32(box + 4, entries) && count && count <= 256) {
    bool bad = false;
    for (uint32_t i = 0; i < count && !bad; ++i) {
      uint32_t v = 0, vt = 0, fn = 0;
      if (!GuestU32(entries + 8 * i + 4, v) || !v) continue;
      if (!GuestU32(v, vt) || !GuestU32(vt + 4, fn) || fn < 0x82000000 || fn >= 0x82A00000) bad = true;
    }
    if (bad) {
      ++logged;
      REXLOG_CRITICAL("crash trace: table {:08X} with a freed value (lr {:08X}), {} slot(s):", box, uint32_t(ctx.lr),
                      count);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t k = 0, v = 0, vt = 0, fn = 0;
        if (!GuestU32(entries + 8 * i, k) || !GuestU32(entries + 8 * i + 4, v) || (!k && !v)) continue;
        GuestU32(v, vt);
        GuestU32(vt + 4, fn);
        uint32_t w[6] = {};
        for (int j = 0; j < 6; ++j) GuestU32(v + 4 * j, w[j]);
        REXLOG_CRITICAL("crash trace:   [{}] key |{}| value {:08X} vt {:08X} vt[1] {:08X} words {:08X} {:08X} {:08X} {:08X} {:08X} |{}|",
                        i, GuestText(k + 8, 24), v, vt, fn, w[1], w[2], w[3], w[4], w[5], GuestText(v, 48));
        // a string value: its text through the pointers in its first words
        for (int j = 1; j < 4; ++j)
          if (w[j] >= 0x1000 && w[j] < 0x20000000)
            REXLOG_CRITICAL("crash trace:       word {} -> |{}|", j, GuestText(w[j], 32));
      }
      if (auto logger = rex::GetLogger()) logger->flush();
    }
  }
  // Fix: a value whose vtable[1] is not game code is an object already freed (its
  // memory taken by another): releasing it called garbage (0x20). Its slot is
  // cleared, so the loop below skips it as it skips empty slots (the game's own
  // latent use after free, the TimerStop event's Data: the menu poll's allocations
  // reuse the freed memory; Costumes then METROPOLIS crashed the game, 2026-11-22).
  if (GuestU32(box, count) && GuestU32(box + 4, entries) && count && count <= 4096) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = 0, vt = 0, fn = 0;
      if (!GuestU32(entries + 8 * i + 4, v) || !v) continue;
      if (GuestU32(v, vt) && GuestU32(vt + 4, fn) && fn >= 0x82000000 && fn < 0x82A00000) continue;
      uint8_t* slot = g_membase + entries + 8 * i + 4;
      if (!Readable(slot, 4)) continue;
      std::memset(slot, 0, 4);
      static int skipped = 0;
      if (skipped++ < 20)
        REXLOG_WARN("apt: freed object {:08X} in table {:08X} slot {} not released again", v, box, i);
    }
  }
  __imp__sub_82697398(ctx, base);
}

namespace superman_returns {

void InstallGuestCrashTrace(uint8_t* membase) {
  g_membase = membase;
  std::signal(SIGABRT, OnAbort);
  g_previous_filter = SetUnhandledExceptionFilter(OnUnhandled);
}

}  // namespace superman_returns
