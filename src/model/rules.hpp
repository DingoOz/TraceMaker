// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Design rules as KiCad defines them (design doc 08 §3–4): board minimums and net classes from the
// .kicad_pro project file, plus custom rules from the .kicad_dru file.
#include <cstdint>
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
  bool has_diff_pair_gap = false;  // set explicitly in the project (not inherited)
  int priority = 0;  // lower value = higher priority (KiCad 9+); Default has INT_MAX
  std::string tuning_profile;      // KiCad 10: the name of the class's tuning profile ("" = none)
  bool profile_from_default = false;  // tuning_profile is Default's: the class names none (KiCad's addMissingDefaults)
  // Which of the ten parameters KiCad 10.0.6 fills from Default when unset (NET_SETTINGS::addMissingDefaults) the project sets for
  // this class, one bit each in kNetClassFields order. A net whose classes leave one unset has Default among its effective class's
  // constituents, which hasNetclass('Default') sees.
  std::uint16_t fields = 0;
  static constexpr std::uint16_t kAllFields = 0x3ff;
  // Set by the route job for a differential tuning profile (doc 05 §58): the pair gap its pairs are routed at. Like KiCad's profile rule
  // as KiCad 10.0.6 applies it, it relaxes the clearance between the coupled halves' tracks only, not between a track and the partner's
  // pads (the class's diff_pair_gap still does that). 0 = none.
  Coord tuning_pair_gap = 0;
};

// A KiCad 10 tuning profile (`.kicad_pro` `tuning_profiles`, doc 05 §58): per signal layer the track width and pair gap of the
// class's nets there (0 = not set) and the propagation delay per millimetre (KiCad's time units; used only with time-domain tuning).
struct TuningLayer {
  std::string layer;       // signal layer name, e.g. "F.Cu"
  Coord width = 0, gap = 0;
  std::int64_t delay = 0;
};
// A via's delay for one signal-layer and via-layer span, instead of the profile's delay per mm (layer names as the
// project gives them; KiCad matches them exactly, upper layer first).
struct TuningViaOverride {
  std::string signal_from, signal_to, via_from, via_to;
  std::int64_t delay = 0;  // attoseconds
};
struct TuningProfile {
  std::string name;
  bool differential = false;  // KiCad's PROFILE_TYPE DIFFERENTIAL (1); SINGLE (0) otherwise
  bool time_domain = false;
  std::vector<TuningLayer> layers;
  std::int64_t via_delay = 0;  // attoseconds per mm of via height
  std::vector<TuningViaOverride> via_overrides;
  const TuningViaOverride* via_override(const std::string& sf, const std::string& st, const std::string& vf, const std::string& vt) const {
    for (const auto& o : via_overrides)
      if (o.signal_from == sf && o.signal_to == st && o.via_from == vf && o.via_to == vt) return &o;
    return nullptr;
  }
  // The width set for `layer`, 0 if none.
  Coord width_on(const std::string& layer) const {
    for (const auto& l : layers)
      if (l.layer == layer) return l.width;
    return 0;
  }
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
  Coord solder_mask_min_width = 0;  // minimum mask web between openings (KiCad solder_mask_bridge test)
  int min_resolved_spokes = 2;      // thermal spokes a pad needs into its zone (KiCad starved_thermal; project rules.min_resolved_spokes)
  bool allow_blind_buried_vias = false;
  bool allow_microvias = false;
};

// One constraint inside a custom rule, e.g. (constraint clearance (min 0.2mm)).
struct Constraint {
  std::string type;                         // clearance, track_width, disallow, …
  std::optional<Coord> min, opt, max;       // lengths in nm (angles/counts are stored ×1e6 as well)
  std::vector<std::string> items;           // disallow item types, or other bare words
  bool time = false;                        // length or skew given in ps or fs: min/opt/max are attoseconds (KiCad's time domain)
};

struct CustomRule {
  std::string name;
  std::string condition;                    // raw expression text, compiled by the rule engine
  std::string layer;                        // "", "outer", "inner", or a layer name
  std::string severity;
  std::vector<Constraint> constraints;
  bool implicit = false;  // one of the rules KiCad compiles itself (tuning profiles, doc 05 §58), not from the .kicad_dru
};

struct DesignRules {
  BoardMinimums minimums;
  std::vector<NetClass> classes;            // classes[0] is "Default"
  std::vector<std::pair<std::string, std::string>> patterns;   // (net-name pattern, class name), in file order
  std::map<std::string, std::vector<std::string>> assignments; // explicit net → class names
  std::vector<CustomRule> custom;
  std::map<std::string, std::string> severities;  // KiCad violation type -> error | warning | ignore
  std::vector<TuningProfile> tuning_profiles;     // KiCad 10 tuning profiles, as the project lists them
  // KiCad's "Include stackup height in track length calculations" (on unless the project turns it off): vias add
  // the stackup height between the layers they join to a net's length (drc/length.hpp, via_heights).
  bool use_height_for_length = true;
  // Router only, never read from a file: added to every clearance between a via and other copper. The relaxed stages of a
  // clearance ramp lower all clearances; with this set back to the difference, vias keep the board's own clearance while
  // tracks route thin (doc 05 §67: a thin sketch's tracks thicken, its vias parked in cells too small for them do not).
  Coord via_clearance_extra = 0;
  // Router only: added to the clearance between a new via and fixed copper (the board's pads, holes' copper, tracks, vias, zones), not
  // routed copper. A relaxed stage's vias then keep the board's clearance to what legalising cannot move (doc 20 §2.6).
  Coord via_fixed_clearance_extra = 0;
  std::vector<std::string> warnings;        // anything that could not be interpreted

  const NetClass& default_class() const { return classes.front(); }
  // Effective net class for a net name (explicit assignment, then the first matching pattern, else Default).
  const NetClass& class_for(const std::string& net_name) const;
  const NetClass* find_class(const std::string& name) const;
  // Names of every class that applies to a net (explicit assignments, then matching patterns), and "Default", which
  // KiCad 10.0.6 counts in every net's effective class: the constituents hasNetclass() searches.
  std::vector<std::string> classes_of(const std::string& net_name) const;
  // The tuning profile a class names, if the project has one of that name.
  const TuningProfile* profile_of(const NetClass& c) const {
    for (const auto& p : tuning_profiles)
      if (!c.tuning_profile.empty() && p.name == c.tuning_profile) return &p;
    return nullptr;
  }
};

// KiCad net-class pattern match: shell-style wildcards (* and ?), case-sensitive.
bool wildcard_match(std::string_view pattern, std::string_view text);
// Net-class pattern: wildcard match or full regular-expression match (KiCad accepts both).
bool pattern_match(const std::string& pattern, const std::string& text);

}  // namespace tmk::model
