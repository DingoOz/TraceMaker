// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Resolves KiCad constraints for pairs of copper items (design doc 03 §6): net-class clearances, local pad
// overrides, board minimums and custom .kicad_dru rules with their conditions.
#include <tuple>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "drc/copper.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

class Condition;  // compiled custom-rule condition

class RuleEngine {
 public:
  RuleEngine(const model::Board& b, const model::DesignRules& r);
  ~RuleEngine();

  // Required copper-to-copper clearance between two items on a copper layer.
  Coord clearance(const CopperItem& a, const CopperItem& b, int layer) const;
  // Required clearance between a hole and a copper item.
  Coord hole_clearance(const CopperItem* hole_owner, const CopperItem& other, int layer) const;
  Coord hole_to_hole(const CopperItem* a, const CopperItem* b) const;
  Coord edge_clearance(const CopperItem& a, int layer) const;
  // Minimum and maximum track width (max = 0: none).
  std::pair<Coord, Coord> track_width(const CopperItem& t, int layer) const;
  Coord via_diameter_min(const CopperItem& v) const;
  Coord annular_width_min(const CopperItem& v) const;
  Coord hole_size_min(const CopperItem* owner) const;
  // Largest clearance any pair could need (for spatial-index inflation).
  Coord max_clearance() const { return max_clearance_; }
  // KiCad applies a copper zone's own clearance to its fill (ZONE::GetLocalClearance): the larger of it and the
  // net-class clearance, unless a pad override or custom rule decides. The DRC enables it; the router does not
  // yet (D51: changing it needs a routing benchmark run).
  void use_zone_clearance_overrides();

  const model::NetClass& netclass(const CopperItem& it) const;
  // True if the two nets are the P/N (or +/-) halves of one differential pair.
  bool coupled_diff_pair(model::NetId a, model::NetId b) const;
  // Length constraint (min, max) of the last custom rule whose condition matches a track of `net`, if any.
  std::pair<std::optional<Coord>, std::optional<Coord>> length_constraint(model::NetId net) const;
  // Maximum skew of the last custom rule with a `skew` constraint matching a track of `net`, if any.
  std::optional<Coord> skew_constraint(model::NetId net) const;
  // Last custom constraint of `type` (e.g. diff_pair_gap, diff_pair_uncoupled) whose rule matches a track of `net`.
  std::optional<model::Constraint> net_constraint(model::NetId net, const std::string& type) const;

  // Name of the last rule disallowing `it` on `layer`, if any (KiCad: items_not_allowed).
  std::optional<std::string> disallowed(const CopperItem& it, int layer) const;
  // Router checks for a new track or through via. Positional and footprint conditions are warned and left to DRC.
  bool track_allowed(model::NetId net, int layer) const;
  bool via_allowed(model::NetId net) const;
  // Hole-to-copper clearance on `layer`, any net; -1 when no rule matches (KiCad: hole_clearance).
  Coord physical_hole_clearance(const CopperItem* hole_owner, const CopperItem& other, int layer) const;
  bool any_physical_hole_clearance() const { return max_physical_hole_ > 0; }
  Coord max_physical_hole_clearance() const { return max_physical_hole_; }
  // Custom rules that the per-class obstacle cache cannot represent require exact per-point checks.
  bool needs_exact_routing() const { return needs_exact_; }
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  struct Compiled {
    const model::CustomRule* rule;
    std::unique_ptr<Condition> cond;  // null = always
    bool valid = true;
    bool positional = false;  // position, footprint or pad condition: cannot be pre-evaluated for routing
  };
  // Item type, layer and condition all match a disallow constraint.
  bool disallow_hit(const Compiled& c, const CopperItem& it, int layer) const;
  // Value of the last matching custom constraint of `type` (min field), trying (a,b) and (b,a).
  std::optional<Coord> custom_min(const char* type, const CopperItem* a, const CopperItem* b, int layer) const;
  bool layer_matches(const std::string& sel, int layer) const;

  const model::Board& b_;
  const model::DesignRules& r_;
  std::vector<Compiled> rules_;
  std::vector<std::string> warnings_;
  Coord max_clearance_ = 0;
  std::vector<const model::NetClass*> net_class_;  // by net id (nets created later fall back to a lookup)
  std::vector<model::NetId> dp_partner_;          // by net id: the other half of a P/N pair, or 0
  // Area functions (insideArea, intersectsArea, enclosedByArea) of a zone fill, by (condition node, zone, fill):
  // a fill has tens of thousands of points and is asked again for every pair it is in (vme-wren: 80 s of a DRC).
  mutable std::map<std::tuple<const void*, int, int>, bool> area_cache_;
  mutable std::mutex area_mutex_;
  bool any_custom_clearance_ = false;
  bool zone_overrides_ = false;
  bool needs_exact_ = false;
  Coord max_physical_hole_ = 0;
  friend class Condition;
};

}  // namespace tmk::drc
