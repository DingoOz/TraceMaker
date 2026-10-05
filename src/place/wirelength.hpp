#pragma once
// Wirelength and airwire metrics: weighted half-perimeter wirelength (HPWL) and crossings between the
// minimum-spanning-tree airwires of different signal nets (doc 04 §3 E).
#include <array>
#include <cstdint>
#include <vector>

#include "place/legality.hpp"

namespace tmk::place {

struct Seg {
  Point a, b;
  int net = -1;
};

// HPWL of one net (unweighted, nm).
Coord net_hpwl(const Problem& p, const Placement& pl, int net);
// Σ weight · HPWL over all nets (weights kSignalWeight / kPowerWeight).
std::int64_t weighted_hpwl(const Problem& p, const Placement& pl);
// Σ HPWL over all nets, unweighted (nm).
std::int64_t total_hpwl(const Problem& p, const Placement& pl);

// Via estimate of the side assignment (doc 04 §3 C, D48): a net whose surface-mount pins sit on both sides needs at
// least one via per pin on its minority side, so it counts min(front, back) such pins. Through-hole pins reach
// both sides and do not count; pseudo-nets never count. `weighted`: Σ weight · min(...), else Σ min(...).
std::int64_t side_vias(const Problem& p, const Placement& pl, bool weighted);
// Front/back surface-mount pin counts of one net in `pl` (as used by side_vias).
std::array<int, 2> side_counts(const Problem& p, const Placement& pl, int net);

// Minimum spanning tree of the pins (Prim, Manhattan metric, ties by pin order) appended to `out`.
void net_mst(const std::vector<Point>& pts, int net, std::vector<Seg>& out);
// True if the two segments cross at a single interior point (touching or collinear overlap does not count).
bool proper_cross(const Seg& s, const Seg& t);
// MST airwires of every signal net.
std::vector<Seg> airwires(const Problem& p, const Placement& pl);
// Number of crossing pairs of airwires that belong to different signal nets (reference: O(n²)).
std::int64_t count_crossings(const Problem& p, const Placement& pl);

}  // namespace tmk::place
