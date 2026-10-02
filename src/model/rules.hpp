#pragma once
// Design rules as KiCad defines them (design doc 08 §3–4): board minimums and net classes from the
// .kicad_pro project file, plus custom rules from the .kicad_dru file.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/units.hpp"

namespace tmk::model {

struct NetClass {
  std::string name;
  Coord clearance = 200'000;
  Coord track_width = 250'000;
  Coord via_diameter = 600'000;
  Coord via_drill = 300'000;
  Coord uvia_diameter = 300'000;
  Coord uvia_drill = 100'000;
  Coord diff_pair_width = 200'000;
  Coord diff_pair_gap = 250'000;
  Coord diff_pair_via_gap = 250'000;
  int priority = 0;  // lower value = higher priority (KiCad 9+); Default has INT_MAX
};

struct BoardMinimums {
  Coord clearance = 0;
  Coord track_width = 0;
  Coord connection_width = 0;
  Coord via_diameter = 0;
  Coord via_annular_width = 0;
  Coord through_hole_diameter = 0;   // minimum drill
  Coord microvia_diameter = 0, microvia_drill = 0;
  Coord hole_clearance = 0;
  Coord hole_to_hole = 0;
  Coord copper_edge_clearance = 0;
  Coord silk_clearance = 0;
  Coord solder_mask_to_copper_clearance = 0;
  bool allow_blind_buried_vias = false;
  bool allow_microvias = false;
};

// One constraint inside a custom rule, e.g. (constraint clearance (min 0.2mm)).
struct Constraint {
  std::string type;                         // clearance, track_width, disallow, …
  std::optional<Coord> min, opt, max;       // lengths in nm (angles/counts are stored ×1e6 as well)
  std::vector<std::string> items;           // disallow item types, or other bare words
};

struct CustomRule {
  std::string name;
  std::string condition;                    // raw expression text, compiled by the rule engine
  std::string layer;                        // "", "outer", "inner", or a layer name
  std::string severity;
  std::vector<Constraint> constraints;
};

struct DesignRules {
  BoardMinimums minimums;
  std::vector<NetClass> classes;            // classes[0] is "Default"
  std::vector<std::pair<std::string, std::string>> patterns;   // (net-name pattern, class name), in file order
  std::map<std::string, std::vector<std::string>> assignments; // explicit net → class names
  std::vector<CustomRule> custom;
  std::vector<std::string> warnings;        // anything that could not be interpreted

  const NetClass& default_class() const { return classes.front(); }
  // Effective net class for a net name (explicit assignment, then the first matching pattern, else Default).
  const NetClass& class_for(const std::string& net_name) const;
  const NetClass* find_class(const std::string& name) const;
};

// KiCad net-class pattern match: shell-style wildcards (* and ?), case-sensitive.
bool wildcard_match(std::string_view pattern, std::string_view text);

}  // namespace tmk::model
