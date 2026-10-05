// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Escape planning, version 2 (roadmap M9; design doc 05 §3 and §14): min-cost-flow channel assignment for deep
// ball-grid arrays, layer by layer.
//
// Version 1 (escape.hpp) sends perimeter pins straight out and every inner ball to a fixed dog-bone via site. In
// an array deeper than two rings that leaves the channels between balls unassigned: which inner ball uses which
// gap, on which layer, is decided by whichever net's search runs first. Here each deep array gets the
// network-flow escape model with diagonal capacities (Yan & Wong, "A correct network flow model for escape
// routing", DAC 2009): the square gaps between four balls are graph nodes, the gap between two neighbouring balls
// is an edge whose capacity is the number of tracks that fit there, and every gap node carries a capacity from its
// diagonals, which every track turning in or crossing the gap must pass (we take the narrower diagonal for both,
// which is conservative). A min-cost max flow from the balls to the outside of the array gives each escaping ball
// a channel sequence. Layers are assigned one after another, as in the layer-by-layer escape of Ozdal & Wong
// ("Algorithms for simultaneous escape routing and layer assignment of dense PCBs", TCAD 2006) and Lin et al.
// (DAC 2021): the pad layer first; the balls that cannot leave on it get a
// dog-bone via site (an assignment, also by min-cost flow, to interstitial sites no pad-layer escape crosses);
// then each further layer routes the remaining vias out between the via sites, which are that layer's obstacles.
//
// The result is a set of corridors the router reserves exactly as version 1's: reservations only remove options
// from other nets, every commit is still checked exactly, so the plan cannot create violations. Integer costs,
// pads and layers in board order and fixed arc order make it deterministic.
#include <functional>
#include <vector>

#include "route/escape.hpp"

namespace tmk::route {

struct FlowEscapeInput {
  // Planning rules per net (the router's class width, clearance and via diameter). An array is planned with the
  // most common values among its pins (ties: the narrower), so one power net does not shrink every channel.
  std::function<Coord(model::NetId)> width, clearance, via;
  std::function<Coord(model::NetId)> keep;  // corridor band, as for version 1 (track width + clearance)
  // Optional checks against fixed copper (absent: geometry of the array alone). track_free(layer, p, net): a
  // track of that net's planning width centred at p is clear of fixed copper on `layer`; via_free(p, net): that
  // net's via fits at p.
  std::function<bool(int, geom::Point, model::NetId)> track_free;
  std::function<bool(geom::Point, model::NetId)> via_free;
  int layers = 2;  // copper layers of the board
};

// Per ring (1 = perimeter) of the planned arrays: how the pins that need routing were assigned.
struct RingPlan {
  int pins = 0;
  int pad_layer = 0;     // escape on the pad layer through planned channels
  int other_layer = 0;   // dog-bone via, then planned channels on another layer
  int via_only = 0;      // dog-bone via site, but no layer had room to leave the array
  int none = 0;          // no channel and no via site
};
struct FlowEscapeStats {
  int arrays = 0;
  std::vector<RingPlan> rings;  // index 0 = ring 1
  std::vector<int> per_layer;   // escapes planned per copper layer
};

// Deep arrays: SMD pads of one footprint on a regular square grid (every pad within pitch / 8 of a grid point,
// at most one pad per point, at least 30 % of the points populated, at least 5 x 5) with at least one pin that
// needs routing in the third ring or deeper. Pads larger than the pitch (thermal pads) are left to the fixed
// copper checks. ring_of(pad) is the pad's ring (1 = perimeter) for pads of deep arrays, 0 otherwise.
std::vector<int> array_rings(const model::Board& b, const std::vector<char>& needs, const EscapeOptions& o = {});

// Plans the deep arrays by min-cost flow and every other dense package as version 1 does (plan_escapes).
std::vector<EscapeCorridor> plan_escapes_flow(const model::Board& b, const std::vector<char>& needs, const FlowEscapeInput& in,
                                              const EscapeOptions& o = {}, FlowEscapeStats* stats = nullptr);

}  // namespace tmk::route
