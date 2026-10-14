// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Gridless legalisation of routed copper (doc 05 §41.3, D104).
//
// The lattice router places every track on a lattice point and cannot move one it has committed, so a solution found under
// looser rules (smaller clearances) usually has room to spare that the lattice cannot use. This module takes such routes and
// moves the vertices of their tracks and vias (the topology, layers and order along every channel stay as they are) until
// every clearance of the board's real rules holds, or as many as it can: a vertex is pushed along the normal of each pair of
// copper items that are too close, by Gauss-Seidel sweeps over the pairs (position-based dynamics, Müller, Heidelberger,
// Hennix and Ratcliff, J. Vis. Commun. Image Represent. 18(2), 2007). A prototype that solved the same problem by sequential
// linear programming (HiGHS, minimum total movement) did about as well (6 stuck srj20 boards, 0.06 mm solutions to 0.10 mm: LP 3
// legal and 3-11 pairs left on the others, projection 2 legal and 2-9 left). What remains
// violated is dropped and routed again by the caller, and every route that is kept passes the router's exact check, so the
// heuristic can lose work but never legality.
#include <cstdint>
#include <vector>

#include "geom/point.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/router.hpp"

namespace tmk::route {

struct LegalizeOptions {
  double box_mm = 0.25;      // a vertex stays within this distance of where it started
  double margin_mm = 0.003;  // clearance asked for beyond the rule (rounding to nanometres, the pairs between sweeps)
  int sweeps = 300;          // projection sweeps at most
  double edge_mm = 0.035;    // copper stays this far inside the board outline (plus the rule's edge clearance)
  std::uint64_t seed = 0;    // != 0: the pairs are visited in another (seeded) order, which leaves other pinches unresolved: portfolio variants differ
  std::vector<geom::Point> outline;  // board outline polygon (Obstacles::outline()); empty = unbounded
};

struct LegalizeResult {
  int nodes = 0;              // vertices of the routes (tracks and vias merged by net and position)
  int moved = 0;              // vertices that ended up elsewhere
  int pairs_before = 0;       // pairs of copper items closer than the rules allow, before and after
  int pairs_after = 0;
  double max_move_mm = 0;
  int sweeps_used = 0;
};

// Moves the vertices of `routes` (tracks and vias of the connections) so that the clearances of `rules` hold between nets, to
// the pads of `board` and to its fixed tracks and vias. Locked or foreign copper never moves; track ends at the centre of
// their own pad stay. Deterministic.
LegalizeResult legalize_routes(const model::Board& board, const model::DesignRules& rules, std::vector<ConnRoute>& routes,
                               const LegalizeOptions& opt = {});

}  // namespace tmk::route
