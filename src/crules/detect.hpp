#pragma once
// Category detection and role binding (design doc 15 §3): each footprint is scored against every category's
// detectors (integer weights, confidence = min(100, sum)); a category instance is the anchor footprint plus the
// parts, pads and nets that play each role. Pure function of the board, the catalogue and the user override file;
// output in catalogue order, then natural reference order (doc 15 §2.4).
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crules/catalogue.hpp"
#include "crules/overrides.hpp"
#include "crules/topology.hpp"
#include "model/board.hpp"

namespace tmk::crules {

struct Role {
  std::string name;
  std::vector<int> parts;            // footprint indices
  std::vector<int> pads;             // pad indices (pins playing the role, e.g. an IC's XTAL pins)
  std::vector<model::NetId> nets;
  bool empty() const { return parts.empty() && pads.empty() && nets.empty(); }
};

// What the user override file (doc 15 §6.3) changed for one rule of one instance.
struct RuleOverride {
  std::string rule;                  // rule id
  bool disabled = false;
  bool has_params = false;
  nlohmann::ordered_json params;     // the rule's parameters after `set` entries (valid when has_params)
  std::vector<std::string> notes;    // "disabled (disable XTAL-04@Y1)", "max_mm = 5 (catalogue 2; set XTAL-02 ...)"
};

struct Instance {
  int category = -1;                 // index into Catalogue::categories
  int anchor = -1;                   // footprint index
  int confidence = 0;                // 0..100
  bool capped = false;               // confidence capped because the board has no pin names (doc 15 §3.6)
  std::vector<std::string> evidence; // matched detectors, e.g. "lib_id +60"
  std::vector<Role> roles;           // bound roles, binding order
  std::string superseded_by;         // set when a conflicting category won the same anchor (doc 15 §3.4)
  bool asserted = false;             // forced by the user override file (`assert`)
  std::vector<RuleOverride> overrides;  // from `disable` and `set` entries, one per rule id
  const Role* role(std::string_view name) const;
  const RuleOverride* override_for(std::string_view rule) const;
  bool disabled(std::string_view rule) const;
};

struct Detection {
  std::vector<Instance> instances;   // confidence >= suggest threshold, not superseded
  std::vector<Instance> possible;    // below the suggest threshold (or superseded): reported only
  bool board_has_pin_names = false;
  // User override file: its name, its entries (as text) and the entries that matched no instance on this board.
  std::string override_source;
  std::vector<std::string> override_entries, override_unused;
};

// The parameters of rule `r` for instance `in`: the catalogue's, or the user's after `set` entries (doc 15 §6.3).
const nlohmann::ordered_json& rule_params(const Instance& in, const RuleSpec& r);

// Confidence cap when the board carries no pad pin names (KiCad 5 files): doc 15 §3.6.
inline constexpr int kNoPinNameCap = 60;
// Instances below this confidence are not even listed as "possible" (they would list every J* and U*).
inline constexpr int kPossibleMin = 20;

// With `ov` (the user override file, doc 15 §3.5 level 1): references are checked against the board (throws on
// unknown ones); `deny` removes a category from a part (listed as possible, so a competing category can win);
// `assert` forces one (confidence 100, wins conflicts; bound by the category's binder, or to the anchor alone when
// the binder rejects the part); `disable` and `set` are attached to the instances: entries for one reference
// (@REF) take precedence over global ones, and among equals the later entry wins.
Detection detect(const model::Board& b, const Catalogue& cat, const Overrides* ov = nullptr);

}  // namespace tmk::crules
