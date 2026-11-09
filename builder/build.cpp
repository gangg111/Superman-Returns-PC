#include "build.h"

#include <sstream>
#include <vector>

#include "audio.h"
#include "delta.h"
#include "pack.h"
#include "textures.h"
#include "xiso.h"

namespace srb {

namespace {

// builder.txt in the payload (tools/builder/make_builder.py):
//   game <path on the disc> <SHA-256>          a file of the game, copied to game\<path>
//   patch <source path> <payload patch> <target path>   game\<target> from game\<source>
struct Plan {
  std::vector<std::pair<std::string, std::string>> game;  // path, sha
  struct Patch {
    std::string source, patch, target;
  };
  std::vector<Patch> patches;
};

Plan ReadPlan(const Pack& pack) {
  const std::vector<uint8_t> text = pack.Get("builder.txt");
  std::istringstream in(std::string(text.begin(), text.end()));
  Plan plan;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream words(line);
    std::string kind;
    words >> kind;
    if (kind == "game") {
      std::string path, sha;
      words >> path >> sha;
      plan.game.emplace_back(path, sha);
    } else if (kind == "patch") {
      Plan::Patch p;
      words >> p.source >> p.patch >> p.target;
      plan.patches.push_back(p);
    }
  }
  return plan;
}

uint64_t FreeBytes(const fs::path& dir) {
  ULARGE_INTEGER free{};
  GetDiskFreeSpaceExW(dir.c_str(), &free, nullptr, nullptr);
  return free.QuadPart;
}

}  // namespace

fs::path Build(const Pack& pack, const BuildOptions& options, std::atomic<bool>& cancel,
               const std::function<void(Step, double, const std::string&)>& progress) {
  auto check_cancel = [&] {
    if (cancel) throw Failure("cancelled");
  };
  progress(Step::Check, 0, "");
  const Plan plan = ReadPlan(pack);

  GameSource game;
  try {
    game.Open(options.source);
  } catch (const std::exception&) {
    throw Failure("not_game");
  }
  uint64_t game_bytes = 0;
  for (auto& [path, sha] : plan.game) {
    uint64_t size = 0;
    if (!game.Find(path, nullptr, &size)) throw Failure("missing:" + path);
    game_bytes += size;
  }
  uint64_t port_bytes = 0;
  for (auto& [path, f] : pack.Files())
    if (path.rfind("files/", 0) == 0) port_bytes += f.size;

  fs::create_directories(options.output);
  const fs::path final_dir = options.output / kGameFolder;
  const fs::path work = options.output / (std::wstring(kGameFolder) + L".part");
  // the sounds as 16-bit WAV take about 1.7 times the game's DATA (measured: 3.6 GB from 2.1 GB)
  if (FreeBytes(options.output) < game_bytes * 3 + port_bytes + (256ull << 20)) throw Failure("space");
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);

  try {
    // the port: every payload file under files/
    uint64_t done = 0;
    for (auto& [path, f] : pack.Files()) {
      if (path.rfind("files/", 0) != 0) continue;
      check_cancel();
      progress(Step::Port, double(done) / double(port_bytes + 1), path.substr(6));
      const std::vector<uint8_t> data = pack.Get(path);
      WriteAll(work / fs::path(Widen(path.substr(6))), data.data(), data.size());
      done += f.size;
    }

    // the game, each file checked against the release the port is made for
    done = 0;
    for (auto& [path, sha] : plan.game) {
      check_cancel();
      std::string real;
      game.Find(path, &real, nullptr);
      Sha256 h;
      game.Copy(real, work / L"game" / fs::path(Widen(path)), h, [&](uint64_t n) {
        done += n;
        progress(Step::Game, double(done) / double(game_bytes + 1), path);
        check_cancel();
      });
      if (h.Hex() != sha) throw Failure("wrong_game:" + path);
    }

    // the menus' archives: our patches on the player's files
    int i = 0;
    for (auto& p : plan.patches) {
      check_cancel();
      progress(Step::Patch, double(i++) / double(plan.patches.size()), p.target);
      const std::vector<uint8_t> source = ReadAll(work / L"game" / fs::path(Widen(p.source)));
      std::vector<uint8_t> target;
      try {
        target = ApplyDelta(source, pack.Get(p.patch));
      } catch (const std::runtime_error& e) {
        if (std::string(e.what()) == "source") throw Failure("wrong_game:" + p.source);
        throw;
      }
      WriteAll(work / L"game" / fs::path(Widen(p.target)), target.data(), target.size());
    }

    // the remastered textures: ours, and the ones made from the player's game
    if (options.textures && pack.Has("textures.txt")) {
      const int n = MakeTextures(pack, work / L"game", work / L"textures", cancel,
                                 [&](double f, const std::string& name) { progress(Step::Textures, f, name); });
      progress(Step::Textures, 1, std::to_string(n));
    }

    // the native audio engine's sounds (when the builder carries FFmpeg)
    bool has_ffmpeg = false;
    for (auto& [path, f] : pack.Files()) has_ffmpeg |= path.rfind("ffmpeg/", 0) == 0;
    if (has_ffmpeg) {
      wchar_t tmp[MAX_PATH];
      GetTempPathW(MAX_PATH, tmp);
      const fs::path ff = fs::path(tmp) / L"SupermanReturnsBuilder" / L"ffmpeg";
      for (auto& [path, f] : pack.Files()) {
        if (path.rfind("ffmpeg/", 0) != 0) continue;
        const std::vector<uint8_t> data = pack.Get(path);
        WriteAll(ff / fs::path(Widen(path.substr(7))), data.data(), data.size());
      }
      const AudioResult a = ConvertAudio(work / L"game" / L"DATA", work / L"native_audio", ff, cancel,
                                         [&](double f, const std::string& name) { progress(Step::Audio, f, name); });
      progress(Step::Audio, 1, std::to_string(a.exact) + "/" + std::to_string(a.sounds));
    }

    // in place of an older build: the player's settings kept
    progress(Step::Finish, 0, "");
    if (fs::exists(final_dir)) {
      const fs::path settings = final_dir / L"superman_returns.toml";
      if (fs::exists(settings)) fs::copy_file(settings, work / L"superman_returns.toml", fs::copy_options::overwrite_existing);
      const fs::path old = options.output / (std::wstring(kGameFolder) + L".old");
      fs::remove_all(old, ec);
      fs::rename(final_dir, old);
      fs::rename(work, final_dir);
      fs::remove_all(old, ec);
    } else {
      fs::rename(work, final_dir);
    }
    progress(Step::Finish, 1, "");
    return final_dir;
  } catch (...) {
    fs::remove_all(work, ec);
    throw;
  }
}

}  // namespace srb
