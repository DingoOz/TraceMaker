// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Escape (fanout) planning, version 1 (roadmap M9; design doc 05 §3).
//
// Dense packages (QFP/TSOP/SOIC rows, BGA/LGA arrays) fail in the router's strict first pass when other nets'
// routes take the channels a pin needs to leave its package: the pin is then "boxed in" until negotiation,
// which large boards often do not reach within the budget. The plan reserves, for every pin that has to be
// routed, a short escape corridor: straight out of the package for pins on its perimeter, and a dog-bone stub to
// the diagonal interstitial via site for the inner balls of an SMD array (the classic BGA fanout; Yan & Wong,
// "Recent research development in PCB layout", ICCAD 2010, §3; Kong, Yan & Wong, "Optimal simultaneous pin
// assignment and escape routing for dense PCBs", ASP-DAC 2010, for the escape model). The router treats
// another net's corridor as blocked in strict passes and as costly in negotiated passes, and releases a corridor
// once its pin is connected. Reservations only remove options: every commit is still checked exactly, so the
// plan cannot introduce violations.
#include <functional>
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

using geom::Point;

struct EscapeCorridor {
  int pad = -1;                  // board pad index
  model::NetId net = 0;
  int layer = -1;                // copper index the stub runs on
  Point a, b;                    // centreline: pad centre -> escape point (or via site)
  bool has_mid = false;          // second-ring pins: a -> mid (between two outer balls) -> b
  Point mid;
  Coord band = 0;                // half-width of the reserved strip around the centreline
  bool via = false;              // b is a dog-bone via site (reserved on every layer)
  // Version 2 (escape_flow.hpp): the corridor continues from b through these points on tail_layer (the pad's
  // layer, or the layer a dog-bone via changes to).
  std::vector<Point> tail;
  int tail_layer = -1;
};

// Half extents of a pad's bounding box along x and y (rotation applied).
std::pair<Coord, Coord> pad_half_extents(const model::Pad& p);

struct EscapeOptions {
  int min_pads = 8;              // smaller footprints escape easily
  Coord max_pitch = 1'300'000;   // pin pitch up to which a footprint counts as dense (1.27 mm BGAs included)
  // Corridor length past the pad edge for perimeter pins. One variant, fixed budget: 0.5 / 1 / 2 / 3 mm gave
  // logicbone 961 / 964 / 968 / 977 and decelerator 449 / 479 / 491 / 490 routed; 2 mm reserves less board
  // area than 3 mm for nearly the same gain (doc 05 §12).
  Coord length = 2'000'000;
};

struct EscapeStats {
  int parts = 0, perimeter = 0, second_ring = 0, dogbones = 0;
};

// Plans corridors for the pads with needs[pad] != 0. keep(net) is the distance another net's centreline must
// keep from this net's centreline (track width + clearance); the band is that, capped at a share of the pin
// pitch so the corridors of neighbouring pins never overlap. Deterministic: footprints and pads in board order.
// channel(net), when given, is the gap a track of that net needs between two pads (narrowest legal width plus
// twice the clearance): second-ring balls whose outer neighbours leave that much room escape on their own layer
// between them (the classic two-ring fanout) instead of taking a dog-bone via.
std::vector<EscapeCorridor> plan_escapes(const model::Board& b, const std::vector<char>& needs, const std::function<Coord(model::NetId)>& keep,
                                         const EscapeOptions& o = {}, EscapeStats* stats = nullptr,
                                         const std::function<Coord(model::NetId)>& channel = {});

class Obstacles;

// Escape feasibility (M9 analysis): can each pin of a dense package leave it at all under the board's rules?
// A breadth-first search from the pad over a lattice of the package area, against fixed copper only (nothing
// routed; fills as the obstacles are set: hard, or ignored with set_soft_zones), at the narrowest legal width
// (the router's neck-down width), changing layers wherever that net's via passes every fixed check. The router's
// custom-rule limits apply: per-net track layers and via permission, and position-dependent disallow rules.
// A pin escapes when it reaches `margin` outside the box of the package's pad centres. Pins whose net is already
// complete in the existing copper are `satisfied`, not searched; complete as the router's planner sees it, so with
// soft zones a plane of the net that no pad touches yet leaves the net outstanding. The search is complete over
// its finite lattice and window, so "dead" means "no escape on this lattice": an off-lattice path can exist in
// rare cases (the router keeps trying those pins), and a pin that escapes alone may still lose its channel to its
// neighbours.
struct DeadPin {
  int pad = -1;
  std::string reason;
};
struct PartEscape {
  int footprint = -1;
  std::string ref;
  Coord pitch = 0;
  int pins = 0, escapable = 0;   // pins still to be routed, and those of them that escape
  int satisfied = 0;             // pins whose net is already complete
  std::vector<DeadPin> dead;
  std::string hint;              // what would make the dead pins escapable, when it can be told
};
struct EscapeAnalysisOptions {
  Coord lattice = 40'000;        // search pitch
  Coord margin = 500'000;        // how far outside the package a pin must get
  Coord window = 1'500'000;      // search area around the package
};
std::vector<PartEscape> analyse_escapes(const model::Board& b, const model::DesignRules& r, Obstacles& obs,
                                        const EscapeAnalysisOptions& o = {}, const EscapeOptions& eo = {});

}  // namespace tmk::route
