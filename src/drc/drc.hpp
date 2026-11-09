// SPDX-License-Identifier: GPL-3.0-or-later
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
  model::NetId net = -1;  // unconnected_items: the net that is missing the connection (-1 for other types)
};

struct DrcOptions {
  Coord epsilon = 500;        // KiCad's DRC epsilon (0.0005 mm): gaps below required - epsilon are violations
  bool dangling = true;       // report dangling tracks and vias (warnings)
  bool propagate_nets = true; // check with the nets KiCad assigns on load (drc/connectivity.hpp propagate_nets)
  bool linear_zone_tests = false;  // reference path: test items against every edge of a zone fill (no edge index)
  bool connectivity_only = false;  // only unconnected items (and dangling ends if `dangling`): no clearance or rule checks
  bool lengths_only = false;       // only the `length` and `skew` rules (no clearance, rule or connectivity checks)
};

struct DrcReport {
  std::vector<Violation> violations;
  std::vector<Violation> unconnected;
  std::vector<std::string> warnings;
  std::map<std::string, int> counts() const;
};

DrcReport run_drc(const model::Board& b, const model::DesignRules& r, const DrcOptions& opt = {});

// How far each net is outside its `length` and `skew` rules, as the DRC measures them (doc 05 §59): keyed by the violation
// type and the net's name ("skew_out_of_range /D+"), the amount outside the window in nm or attoseconds. Empty when every net
// is in range or the board has no such rule.
using LengthFaults = std::map<std::string, double>;
LengthFaults length_faults(const model::Board& b, const model::DesignRules& r);

// True when no net is further outside its length or skew window in `with` than in `without` (a net not in a map is in range).
// The after-route stages are judged by it (D173): a stage may not undo the router's tuning.
bool length_faults_no_worse(const LengthFaults& without, const LengthFaults& with);

// Writes the report in kicad-cli's JSON layout (violations, unconnected_items; positions in mm).
void write_drc_json(const DrcReport& rep, const std::string& path);

}  // namespace tmk::drc
