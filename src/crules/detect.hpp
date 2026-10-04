#pragma once
// Category detection and role binding (design doc 15 §3): each footprint is scored against every category's
// detectors (integer weights, confidence = min(100, sum)); a category instance is the anchor footprint plus the
// parts, pads and nets that play each role. Pure function of the board and the catalogue; output in catalogue
// order, then natural reference order (doc 15 §2.4).
#include <string>
#include <string_view>
#include <vector>

#include "crules/catalogue.hpp"
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

struct Instance {
  int category = -1;                 // index into Catalogue::categories
  int anchor = -1;                   // footprint index
  int confidence = 0;                // 0..100
  bool capped = false;               // confidence capped because the board has no pin names (doc 15 §3.6)
  std::vector<std::string> evidence; // matched detectors, e.g. "lib_id +60"
  std::vector<Role> roles;           // bound roles, binding order
  std::string superseded_by;         // set when a conflicting category won the same anchor (doc 15 §3.4)
  const Role* role(std::string_view name) const;
};

struct Detection {
  std::vector<Instance> instances;   // confidence >= suggest threshold, not superseded
  std::vector<Instance> possible;    // below the suggest threshold (or superseded): reported only
  bool board_has_pin_names = false;
};

// Confidence cap when the board carries no pad pin names (KiCad 5 files): doc 15 §3.6.
inline constexpr int kNoPinNameCap = 60;
// Instances below this confidence are not even listed as "possible" (they would list every J* and U*).
inline constexpr int kPossibleMin = 20;

Detection detect(const model::Board& b, const Catalogue& cat);

}  // namespace tmk::crules
