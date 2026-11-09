#pragma once

#include <cstdint>
#include <vector>

namespace superman_returns {

// One entry of a prop manager's list (prop_streaming.cpp): where a prop stands and a key of
// its kind (the entry's id bytes), whether or not the game has created its object.
struct PropEntry {
  float pos[3];
  uint32_t kind;
};

// Every prop manager's entries as of the last scan (twice a second, on the game thread).
std::vector<PropEntry> PropEntries();

}  // namespace superman_returns
