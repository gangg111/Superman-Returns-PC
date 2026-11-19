// Prints the texture slot mask (graphics/guest/shader_slots.h) of every shader container given:
//   shader_slots_dump <file.bin>... -> "<file name> <mask hex>" per line.
// Checked against XenosRecomp's HLSL by tools/shaders/check_shader_slots.py.
#include "../../port/src/graphics/guest/shader_slots.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string name = argv[i];
    if (auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    std::printf("%s %08X\n", name.c_str(), superman_returns::graphics::guest::ShaderTextureSlots(bytes));
  }
}
