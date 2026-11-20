// Prints the texture slot mask (graphics/guest/shader_slots.h) of every shader container given:
//   shader_slots_dump <file.bin>... -> "<file name> <mask hex>" per line.
//   shader_slots_dump --constants <file.bin>... -> "<file name> all" or "<file name> <used[3..0] hex>".
// Checked against XenosRecomp's HLSL by tools/shaders/check_shader_slots.py and check_shader_constants.py.
#include "../../port/src/graphics/guest/shader_slots.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  int first = 1;
  const bool constants = argc > 1 && std::strcmp(argv[1], "--constants") == 0;
  if (constants) first = 2;
  for (int i = first; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string name = argv[i];
    if (auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    if (!constants) {
      std::printf("%s %08X\n", name.c_str(), superman_returns::graphics::guest::ShaderTextureSlots(bytes));
      continue;
    }
    const auto use = superman_returns::graphics::guest::ShaderFloatConstants(bytes);
    if (use.all) std::printf("%s all\n", name.c_str());
    else
      std::printf("%s %016" PRIX64 "%016" PRIX64 "%016" PRIX64 "%016" PRIX64 "\n", name.c_str(), use.used[3], use.used[2],
                  use.used[1], use.used[0]);
  }
}
