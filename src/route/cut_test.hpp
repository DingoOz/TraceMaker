// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Cut test of a routed board's topology (doc 05 §67, D159).
//
// Cole and Siegel (river routing, 1984) and Leiserson and Maley ("Algorithms for routing and testing routability of
// planar VLSI layouts", STOC 1985) show that the wires of a one-layer routing can be moved, keeping their topology, to
// meet given widths and clearances exactly when every cut has room: for every two features (pads, vias) that see each
// other, the wires that must pass between them, with their widths and the clearances between neighbours, fit in the gap.
// Gao, Jerrum, Kaufmann, Mehlhorn and Rülling ("On continuous homotopic one layer routing", 1988) give the condition
// for continuous geometry. This module checks those cuts on every copper layer of a routed board:
//
//  - a cut's wires are the crossings of the routed centrelines with the shortest segment between the two features,
//    less pairs of crossings of one wire that enclose no feature (the wire can be pulled out of the cut);
//  - its need is the sum of the wire widths and of the rule clearances between neighbours along the cut (copper of one
//    net may overlap, so wires of an end's own net next to it and runs of one net merge);
//  - an over-full cut proves that no geometry with this topology and these feature positions meets the rules.
//
// Vias are features of the topology here (the sketch keeps them where they are). The via room test asks the weaker
// question with the via free to move inside its cell (it may not cross a cut between two pads): if no position keeps it
// clear of the fixed pads with the wires the topology puts between them, the via has no room, whatever the geometry.
// Necessary conditions only: wire-to-wire spacing away from features, zones and the board edge are not cuts here.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

struct CutTestOptions {
  Coord max_gap = 2'000'000;   // cuts between features farther apart than this are not checked (they have room)
  Coord tolerance = 500;       // a cut is over-full when its need exceeds its gap by more than this
  bool via_room = true;        // run the via room test on vias whose own position fails a cut
  Coord via_search = 400'000;  // via room test: half-size of the square searched around the via
  Coord via_step = 5'000;      // and its sampling step
};

struct CutFeature {
  bool via = false;
  int index = -1;        // board pad or via index
  model::NetId net = 0;
  model::Point pos;
};

struct OverfullCut {
  int layer = 0;                     // copper index
  CutFeature a, b;
  model::Point pa, pb;               // the cut: closest points of the two features
  Coord gap = 0, need = 0;           // length of the cut and what its wires need
  std::vector<model::NetId> wires;   // nets of the wires that cross it, in order from a
  std::vector<int> wire_tracks;      // for each wire, the board track it crosses on (-1: an arc)
};

struct CutReport {
  int cuts = 0;                    // cuts checked
  int crossings_cancelled = 0;     // crossings removed as pairs that enclose no feature
  double s_star = 1e9;             // smallest clearance scale a cut with a wire or a via allows: (gap - widths) / clearances (>= 1: room)
  std::vector<OverfullCut> overfull;
  std::vector<int> vias_without_room;  // board via indices: no position in the cell keeps clear of the pads (a proof)
};

CutReport cut_test(const model::Board& board, const model::DesignRules& rules, const CutTestOptions& opt = {});

}  // namespace tmk::route
