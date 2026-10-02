#pragma once
// Design-rule checker equivalent to KiCad's for routing-relevant rules (design doc 03 §6). Violation type
// names are KiCad's, so reports compare 1:1 with `kicad-cli pcb drc --format json`.
#include <map>
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

struct ViolationItem {
  std::string description;
  model::Point pos;
};

struct Violation {
  std::string type;
  std::string severity = "error";
  std::string description;
  std::vector<ViolationItem> items;
  Coord actual = -1, required = -1;
  int layer = -1;
};

struct DrcOptions {
  Coord epsilon = 500;        // KiCad's DRC epsilon (0.0005 mm): gaps below required - epsilon are violations
  bool dangling = true;       // report dangling tracks and vias (warnings)
};

struct DrcReport {
  std::vector<Violation> violations;
  std::vector<Violation> unconnected;
  std::vector<std::string> warnings;
  std::map<std::string, int> counts() const;
};

DrcReport run_drc(const model::Board& b, const model::DesignRules& r, const DrcOptions& opt = {});

// Writes the report in kicad-cli's JSON layout (violations, unconnected_items; positions in mm).
void write_drc_json(const DrcReport& rep, const std::string& path);

}  // namespace tmk::drc
