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
#include <string_view>
#include <vector>

#include "drc/copper.hpp"
#include "drc/rule_geometry.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

class Condition;  // compiled custom-rule condition

// A differential-pair net name as KiCad 10.0.6 reads it (DRC_ENGINE::MatchDpSuffix): from the end, digits and underscores are
// skipped, then P or + makes the positive half and N or - the negative one. `complement` is the other half's name and `base` the name
// before the polarity character, so D_P1 pairs with D_N1 on base "D_". Polarity 0: not a pair name.
struct DpSuffix {
  int polarity = 0;
  std::string complement, base;
};
DpSuffix match_dp_suffix(std::string_view name);

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
  // The severity of the last rule with a track_width constraint that matches the track ("" when none matches or it sets none): in KiCad
  // that rule decides the track's width check, so an ignored rule hides the board minimum too (tuning profiles, doc 05 §58).
  std::string track_width_severity(const CopperItem& t, int layer) const;
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
  // KiCad's matched-length check groups items by the rule that gives them a constraint (DRC_ENGINE::EvalRules'
  // parent rule; doc 05 §59): the position in rule order of the last rule with a `type` constraint matching `it` on
  // `layer`, or -1. rule(i) is that rule.
  int last_rule_with(const CopperItem& it, int layer, const std::string& type) const;
  const model::CustomRule& rule(int i) const { return *rules_[static_cast<std::size_t>(i)].rule; }
  // The same for a net as a whole (a track of `net` on any layer), as length_constraint and net_constraint see it.
  int net_rule_with(model::NetId net, const std::string& type) const;
  // The other half of `net`'s differential pair (KiCad's DpCoupledNet), or 0.
  model::NetId dp_partner(model::NetId net) const {
    return static_cast<std::size_t>(net) < dp_partner_.size() ? dp_partner_[static_cast<std::size_t>(net)] : 0;
  }
  // True when `net` is the positive half of a pair (P or +, match_dp_suffix); only meaningful when dp_partner(net) is set.
  bool dp_positive(model::NetId net) const {
    return static_cast<std::size_t>(net) < dp_positive_.size() && dp_positive_[static_cast<std::size_t>(net)];
  }

  // Name of the last rule disallowing `it` on `layer`, if any (KiCad: items_not_allowed).
  std::optional<std::string> disallowed(const CopperItem& it, int layer) const;
  // Router fast paths for a new track or via of `net`, from rules whose conditions do not depend on the
  // item's position, size or footprint (per-net layer and via-type masks).
  bool track_allowed(model::NetId net, int layer) const;
  // `Blind` router probes have no span yet, so either blind/buried keyword conservatively forbids them.
  // Board items are distinguished exactly by whether their span touches an outer copper layer.
  bool via_allowed(model::NetId net, model::ViaType type = model::ViaType::Through) const;
  // A disallow rule on tracks or vias depends on position, size or footprint: the router asks probe_disallowed.
  bool positional_disallow() const { return positional_disallow_; }
  // A new track or via (index -1, actual shape, width and layers; a via's layers are its span, which tells
  // blind from buried) is disallowed on one of its layers, every custom rule applied in order as in disallowed().
  bool probe_disallowed(const CopperItem& it, model::ViaType type = model::ViaType::Through) const;
  // Hole-to-copper clearance on `layer`, any net; -1 when no rule matches (KiCad: hole_clearance).
  Coord physical_hole_clearance(const CopperItem* hole_owner, const CopperItem& other, int layer) const;
  bool any_physical_hole_clearance() const { return max_physical_hole_ > 0; }
  // True when a physical_hole_clearance rule holds between a via of `net` and a track of the same net: the
  // tracks that end in a via touch its hole, so every via of that net would break the rule.
  bool via_hole_rule_hits_own_tracks(model::NetId net) const;
  Coord max_physical_hole_clearance() const { return max_physical_hole_; }
  // Hole-to-copper clearance the router keeps between a hole and other nets' copper: the board minimum raised to
  // every applied custom hole_clearance rule. The router's checks and caches have no per-pair hole rule, so a rule
  // with a condition or a layer is kept everywhere (conservative, rule 6; a warning says so). The DRC stays exact
  // (hole_clearance above).
  Coord router_hole_clearance() const { return router_hole_; }
  // Custom rules that the per-class obstacle cache cannot represent require exact per-point checks.
  bool needs_exact_routing() const { return needs_exact_; }
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  // Keyword bits of a rule's disallow constraints.
  enum : std::uint16_t {
    kTrack = 1, kVia = 2, kThroughVia = 4, kMicroVia = 8, kBlindVia = 16, kBuriedVia = 32, kPad = 64, kZone = 128, kGraphic = 256,
    kRouted = kTrack | kVia | kThroughVia | kMicroVia | kBlindVia | kBuriedVia,
  };
  struct Compiled {
    const model::CustomRule* rule;
    std::unique_ptr<Condition> cond;  // null = always
    bool valid = true;
    bool positional = false;  // position, size, footprint or pad condition: evaluated per item, not per net
    std::uint16_t words = 0;  // disallow keywords
  };
  bool words_match(std::uint16_t words, const CopperItem& it, const model::ViaType* via_type, bool span) const;
  // Item type, layer and condition all match a disallow constraint.
  bool disallow_hit(const Compiled& c, const CopperItem& it, int layer, const model::ViaType* via_type = nullptr, bool span = false) const;
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
  std::vector<char> dp_positive_;                 // by net id: the positive half of a pair
  std::vector<std::string> dp_base_;              // by net id: the pair's base name (inDiffPair), for pair nets
  std::vector<std::vector<std::string>> net_classes_;  // by net id: DesignRules::classes_of
  // Area and courtyard functions of a zone fill, by (condition node, zone, fill): a fill has tens of thousands
  // of points and is asked again for every pair it is in (vme-wren: 80 s of a DRC).
  mutable std::map<std::tuple<const void*, int, int>, bool> area_cache_;
  mutable std::mutex area_mutex_;
  CourtyardCache courtyards_;
  std::vector<geom::Box> zone_box_;  // outline box of every zone, for area functions
  bool positional_disallow_ = false;
  bool any_custom_clearance_ = false;
  bool zone_overrides_ = false;
  bool needs_exact_ = false;
  Coord max_physical_hole_ = 0;
  Coord router_hole_ = 0;
  friend class Condition;
};

}  // namespace tmk::drc
