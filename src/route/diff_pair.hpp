#pragma once
// Differential pairs (design doc 05 §9 phase 3, §14): the geometry rule a pair is routed at, and a measurement of
// how a routed pair came out (coupled share, gap, intra-pair skew). The coupled search itself lives in the router
// (route/router.cpp, `route_pair`), which owns the lattice and the transaction state.
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "drc/rule_engine.hpp"
#include "geom/point.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

// What a pair is routed at. Sources, highest first: a custom rule's `diff_pair_gap` (opt, else min) and
// `diff_pair_uncoupled` (max); the net class's diff-pair width/gap/via gap when the project sets them; else the net
// class width and the clearance between the two halves (the tightest legal coupling). Never below the board minimums
// or the clearance KiCad requires between the halves (the class diff-pair gap relaxes it only for pairs KiCad
// recognises by name, `inDiffPair`).
struct PairRule {
  Coord width = 0;          // track width of each half
  Coord gap = 0;            // edge-to-edge gap of the coupled section (target; >= required)
  Coord required_gap = 0;   // smallest gap the rules allow between the halves
  Coord offset = 0;         // centreline to each track centre: (width + gap) / 2, rounded up, plus a rounding margin
  Coord via_diameter = 0, via_drill = 0;
  Coord via_offset = 0;     // centreline to each via centre in a coupled via pair (>= offset)
  std::optional<Coord> max_uncoupled;  // per half, both ends together
  std::string source;       // "rule", "net class" or "clearance" (for the report)
};

PairRule pair_rule(const model::Board& b, const model::DesignRules& rules, const drc::RuleEngine& re, model::NetId a, model::NetId c);

// Octilinear unit direction d (0..7, E, NE, N, NW, W, SW, S, SE with y down) and its left normal, as doubles.
struct Dir2 { double x, y; };
Dir2 unit_dir(int d);
Dir2 left_normal(int d);
// p + s * v, rounded half away from zero (KiCad's KiROUND), so offsets are symmetric about the centreline.
geom::Point offset_point(geom::Point p, Dir2 v, double s);
// Miter corner of a polyline offset by `s` (signed: + left) where the centreline turns from direction d1 to d2.
geom::Point miter_point(geom::Point c, int d1, int d2, double s);

// A routed pair, measured on its tracks: a sample of one half counts as coupled where a parallel track of the
// other half on the same layer lies within `coupled_gap` edge to edge.
struct PairStats {
  std::string net_a, net_b;
  double length_a = 0, length_b = 0;    // track length, nm
  double coupled_a = 0, coupled_b = 0;  // coupled track length, nm
  int vias_a = 0, vias_b = 0;
  double gap_min = 0, gap_median = 0;   // edge gap over coupled samples, nm (0 when nothing is coupled)
  double coupled_share() const { return length_a + length_b > 0 ? (coupled_a + coupled_b) / (length_a + length_b) : 0.0; }
  double skew() const { return length_a > length_b ? length_a - length_b : length_b - length_a; }
};
// Edge gap up to which a sample counts as coupled: the pair's gap plus a quarter of it (at least 50 um), so lattice
// rounding and the miter at bends do not count as uncoupled while a single track wandering off does.
inline Coord coupled_threshold(const PairRule& r) { return r.gap + std::max<Coord>(r.gap / 4, 50'000); }
PairStats measure_pair(const std::vector<model::Track>& tracks, const std::vector<model::Via>& vias, model::NetId a, model::NetId c,
                       Coord coupled_gap);

// Net pairs KiCad treats as differential pairs (names ending in P/N or +/-), in net order.
std::vector<std::pair<model::NetId, model::NetId>> named_pairs(const model::Board& b, const drc::RuleEngine& re);

}  // namespace tmk::route
