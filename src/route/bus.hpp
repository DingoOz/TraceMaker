// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Buses (design doc 05 §38): groups of nets that belong together (a data bus, an address bus, a parallel display
// interface). This file holds what does not need the router: how a group is named on the command line, how groups
// are found in the net names, which skew limit the board's rules give a group, and the measurement of a routed
// group. The routing of a group lives in the router (route/router.cpp, "buses").
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drc/length.hpp"
#include "drc/rule_engine.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

struct BusGroup {
  std::string name;
  std::vector<model::NetId> members;  // in bus order: natural sort of the net names (D2 before D10)
  std::string source;                 // "option" (--bus) or "detected" (--buses)
  Coord skew_limit = 0;               // largest allowed difference in length between members; 0 = none
  std::string skew_source;            // "rule" (a custom skew rule of the board), "option" (--bus-skew-mm) or empty
};

// Natural order of two names: runs of digits compare as numbers, everything else byte by byte.
bool natural_less(std::string_view a, std::string_view b);

// Bus patterns select nets by their full name (KiCad's, with the sheet path: "/D0", "/MEM/DQ3").
//   [N-M] or [N..M], N <= M decimal numbers: an index range, matching any decimal number in it ("D[0-15]" matches
//                    D0 .. D15, and D07 too). A range needs both bounds; "[[0-7]]" is a range in literal brackets,
//                    for nets named A[0] .. A[7].
//   * and ?:         as in KiCad's net-class patterns (any run of characters, any one character).
// A pattern without an index range is matched exactly as a KiCad net-class pattern (model::pattern_match: wildcards,
// or a full regular expression). In a pattern with a range everything outside the range and the wildcards is literal.
bool bus_pattern_match(const std::string& pattern, const std::string& net_name);

// "NAME=PATTERN" as given to --bus. Throws std::runtime_error when there is no '=', the name or pattern is empty, or
// fewer than two nets with pads match.
BusGroup bus_from_option(const model::Board& b, const std::string& spec);

// Automatic detection (--buses), conservative: nets whose names differ only in a trailing decimal index ("D0", "A[3]",
// "/MEM/DQ12"), at least `min_members` of them with pads on the same two components. KiCad's generated names
// ("Net-(U1-Pad3)", "unconnected-...") are never grouped. One stem may give several groups (D0-7 to one memory,
// D8-15 to another); they are then named STEM(REF-REF). Groups come in natural order of their names.
std::vector<BusGroup> detect_buses(const model::Board& b, int min_members = 4);

// The skew limit the board's custom rules give a group: every member must match a rule with a `skew` constraint
// (the smallest maximum wins); rules restricted to differential pairs (`within_diff_pairs`) do not count.
std::optional<Coord> bus_rule_skew(const drc::RuleEngine& re, const BusGroup& g);

// What a via adds to a net's length (drc/length.hpp, where KiCad's length check lives).
using drc::ViaHeights;
using drc::via_heights;
// The via length of one net on the given copper, as KiCad sums it: each via counts between the outermost layers on
// which a track of the net ends inside the via's pad, or a pad of the net touches the via (a surface pad's own
// layer, its footprint's side for a through-hole pad); a via that joins nothing on a second layer adds nothing.
double net_via_length(const model::Board& b, const ViaHeights& vh, model::NetId net, const std::vector<model::Track>& tracks,
                      const std::vector<model::ArcTrack>& arcs, const std::vector<model::Via>& vias);

// A group as routed, measured on copper. Length is what KiCad's skew check compares: track length (arcs included)
// plus the via length above, so a member with more vias than the others is longer by their height. KiCad also
// adds pad-to-die lengths and shortens a track end inside a pad or via; neither is modelled (micrometres, or zero).
struct BusMember {
  model::NetId net = 0;
  bool routed = false;          // every pad of the net is joined
  double length = 0;            // nm, tracks and vias
  double via_length = 0;        // nm, the vias' part of `length`
  int vias = 0;
  model::LayerMask layers = 0;  // copper layers carrying tracks of the net
  bool followed = false;        // routed with the group's reference via count and layers
};
struct BusStats {
  std::vector<BusMember> members;  // group order
  int routed = 0;
  // The reference is the via count and layer set most members share (ties: the first member's in group order).
  // `followed` members have exactly that; `left` are routed otherwise ("left the bus").
  int followed = 0, left = 0;
  int ref_vias = 0;
  model::LayerMask ref_layers = 0;
  int vias_min = 0, vias_max = 0;       // over routed members
  double length_min = 0, length_max = 0;
  double skew() const { return length_max - length_min; }  // over routed members
};
// `net_routed[net]`: nets_joined() of the board the copper belongs to; `b` gives the pads, `vh` its via heights.
BusStats measure_bus(const model::Board& b, const ViaHeights& vh, const std::vector<model::Track>& tracks, const std::vector<model::ArcTrack>& arcs, const std::vector<model::Via>& vias,
                     const std::vector<std::uint8_t>& net_routed, const BusGroup& g);

// Per net: 1 when all its pads are joined by copper (the router's own connectivity; nets without pads count as joined).
std::vector<std::uint8_t> nets_joined(const model::Board& b, const model::DesignRules& rules);

}  // namespace tmk::route
