// Log of the calls the game makes into its audio engine (EA SndPlayer on
// RenderWare Audio Core), the reference for a native engine: the same scenario
// has to produce the same calls (docs/natywny-dzwiek-plan.md, criterion 1).
// Boundary and signatures: docs/natywny-dzwiek-api.md.
//
// SR_LOG_AUDIO_API=<file> writes one line per call:
//   <ms since start> <guest thread id> <call> <arguments>
// Plug-ins are named "<voice>/<index>:<FourCC>" from Voice::Create, whose
// entries point at class descriptors with the FourCC at +4.
// Every hook runs the original unchanged.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hook.h>

#include "generated/default/superman_returns_init.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Log {
  std::FILE* file = nullptr;
  Clock::time_point start = Clock::now();
  std::mutex mutex;
  // Plug-in pointer -> "voice/index:FourCC".
  std::unordered_map<uint32_t, std::string> plugins;
};

Log& Get() {
  static Log log;
  static std::once_flag opened;
  std::call_once(opened, [] {
    if (const char* path = std::getenv("SR_LOG_AUDIO_API"); path && *path)
      log.file = std::fopen(path, "w");
  });
  return log;
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::string FourCC(uint32_t id) {
  std::string s;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = char((id >> shift) & 0xFF);
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

void Write(Log& log, const PPCContext& ctx, const std::string& line) {
  const double ms =
      std::chrono::duration<double, std::milli>(Clock::now() - log.start).count();
  std::fprintf(log.file, "%.1f %u %s\n", ms, uint32_t(ctx.r13.u32), line.c_str());
}

std::string Plugin(Log& log, uint32_t p) {
  auto it = log.plugins.find(p);
  if (it != log.plugins.end()) return it->second;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", p);
  return buf;
}

}  // namespace

// Voice::Create(count, entries[12 B: init*, ClassDesc*, u8 channels], out Plugin**, sys)
REX_HOOK_RAW(sub_82650398) {
  Log& log = Get();
  const uint32_t count = ctx.r3.u32, entries = ctx.r4.u32;
  __imp__sub_82650398(ctx, base);
  if (!log.file) return;
  const uint32_t voice = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(log.mutex);
  std::string line = "Voice::Create voice=" + Plugin(log, voice) + " chain=";
  char buf[64];
  for (uint32_t i = 0; i < count && i < 32; ++i) {
    const uint32_t desc = Load32(base, entries + i * 12 + 4);
    const std::string fourcc = desc ? FourCC(Load32(base, desc + 4)) : "????";
    const uint32_t channels = base[entries + i * 12 + 8];
    std::snprintf(buf, sizeof(buf), "%s%s(%uch)", i ? ">" : "", fourcc.c_str(), channels);
    line += buf;
    if (voice) {
      // The plug-in pointers sit at voice+72 (the out argument receives &voice+72).
      std::snprintf(buf, sizeof(buf), "%08X/%u:%s", voice, i, fourcc.c_str());
      log.plugins[Load32(base, voice + 72 + i * 4)] = buf;
    }
  }
  Write(log, ctx, line);
}

// Voice::Release(voice)
REX_HOOK_RAW(sub_826505B8) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Voice::Release voice=%08X", ctx.r3.u32);
    Write(log, ctx, buf);
  }
  __imp__sub_826505B8(ctx, base);
}

// Plugin::SetParamF(plugin, index, f1 value)
REX_HOOK_RAW(sub_8264CE48) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[64];
    std::snprintf(buf, sizeof(buf), " param=%u value=%g", ctx.r4.u32, ctx.f1.f64);
    Write(log, ctx, "Plugin::SetParamF " + Plugin(log, ctx.r3.u32) + buf);
  }
  __imp__sub_8264CE48(ctx, base);
}

// Plugin::Control(plugin, index, void* data)
REX_HOOK_RAW(sub_8264CE18) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[96];
    const uint32_t data = ctx.r5.u32;
    if (data)
      std::snprintf(buf, sizeof(buf), " index=%u data=%08X [u32 %08X f32 %g]", ctx.r4.u32, data,
                    Load32(base, data), LoadF(base, data));
    else
      std::snprintf(buf, sizeof(buf), " index=%u data=0", ctx.r4.u32);
    Write(log, ctx, "Plugin::Control " + Plugin(log, ctx.r3.u32) + buf);
  }
  __imp__sub_8264CE18(ctx, base);
}

// Stream::QueueHandle(channel, loop, source) -> request handle
REX_HOOK_RAW(sub_82658078) {
  Log& log = Get();
  const uint32_t ch = ctx.r3.u32, loop = ctx.r4.u32, src = ctx.r5.u32;
  __imp__sub_82658078(ctx, base);
  if (!log.file) return;
  std::lock_guard<std::mutex> lock(log.mutex);
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Stream::Queue ch=%d loop=%d src=%08X -> %08X", int32_t(ch),
                int32_t(loop), src, ctx.r3.u32);
  Write(log, ctx, buf);
}

// Stream::Close(channel)
REX_HOOK_RAW(sub_82657930) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Stream::Close ch=%d", int32_t(ctx.r3.u32));
    Write(log, ctx, buf);
  }
  __imp__sub_82657930(ctx, base);
}

// Stream::SetVolume(channel, f1 volume, speaker)
REX_HOOK_RAW(sub_82656698) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "Stream::SetVolume ch=%d volume=%g speaker=%d",
                  int32_t(ctx.r3.u32), ctx.f1.f64, int32_t(ctx.r4.u32));
    Write(log, ctx, buf);
  }
  __imp__sub_82656698(ctx, base);
}

// Stream::SetPitch(channel, Q12)
REX_HOOK_RAW(sub_82656A40) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Stream::SetPitch ch=%d q12=%d", int32_t(ctx.r3.u32),
                  int32_t(ctx.r4.u32));
    Write(log, ctx, buf);
  }
  __imp__sub_82656A40(ctx, base);
}

// Stream::SetRequestLoop(handle, n)
REX_HOOK_RAW(sub_82658030) {
  Log& log = Get();
  if (log.file) {
    std::lock_guard<std::mutex> lock(log.mutex);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Stream::SetRequestLoop handle=%08X n=%d", ctx.r3.u32,
                  int32_t(int16_t(ctx.r4.u32)));
    Write(log, ctx, buf);
  }
  __imp__sub_82658030(ctx, base);
}

// SndPlayer::IsDone(player) -> u8: logged only when the answer changes per player.
REX_HOOK_RAW(sub_82653E70) {
  Log& log = Get();
  const uint32_t player = ctx.r3.u32;
  __imp__sub_82653E70(ctx, base);
  if (!log.file) return;
  static std::unordered_map<uint32_t, uint32_t> last;
  std::lock_guard<std::mutex> lock(log.mutex);
  const uint32_t done = ctx.r3.u32 & 0xFF;
  auto [it, inserted] = last.try_emplace(player, done);
  if (inserted || it->second != done) {
    it->second = done;
    char buf[32];
    std::snprintf(buf, sizeof(buf), " -> %u", done);
    Write(log, ctx, "SndPlayer::IsDone " + Plugin(log, player) + buf);
  }
}
