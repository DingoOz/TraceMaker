#pragma once
// Deterministic defect injection for DRC parity on broken boards (design doc 03 §6, roadmap M2): takes a
// routed board and adds one kind of defect a given number of times (shorts between nets, dangling tracks and
// vias, cut connections), so TraceMaker's classification of each defect can be compared with kicad-cli's.
#include <cstdint>
#include <string>
#include <vector>

namespace tmk::app {

// Defect kinds in a fixed order.
const std::vector<std::string>& defect_kinds();

// Writes `out` (and a JSON manifest of the injected defects to `manifest`, if not empty). Returns the number
// of defects injected (fewer than `count` when the board offers too few candidates).
int inject_defects(const std::string& in, const std::string& out, const std::string& manifest, const std::string& kind,
                   std::uint64_t seed, int count);

}  // namespace tmk::app
