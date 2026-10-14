// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Where a track is over a void of its reference plane (issue #24, docs/05-routing.md §44, D117).
//
// A fast signal wants solid copper under it on the adjacent layer: where the plane has a split, a slot, a cut-out or an antipad field the return
// current has to go round (docs/05 §42 measures the detour after the route). Here the same question is asked before the route, for every cell of the
// router's lattice: on each copper layer that has a plane next to it (an adjacent layer whose zone fills cover a good part of the board), is the cell's
// centre over the plane's copper? The router charges critical nets for the cells where it is not.
//
// The map is a cost, not a rule: the exact check of every commit is unchanged. Vias a route adds make antipads the fills in the file do not have yet; the
// audit after the route (analysis/return_path.hpp, on a refilled board) sees those.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "geom/clip.hpp"
#include "geom/point.hpp"
#include "model/board.hpp"

namespace tmk::route {

struct ReferenceMap {
  // Per lattice cell, in the router's layout ((layer * ny + y) * nx + x): 1 = the cell is over a void of the layer's reference plane. Empty when no
  // layer has a reference plane.
  std::vector<std::uint8_t> over_void;
  std::vector<int> reference_layer;  // per copper layer: the adjacent layer taken as its reference plane, -1 = none (a plane layer with none counts its own gaps)
  std::vector<double> coverage;      // per copper layer: share of the board's bounding box that its zone fills cover
  bool any() const { return !over_void.empty(); }
};

// `lat` is the lattice box (cell (x, y) is at lat.x0 + x * pitch, lat.y0 + y * pitch), `nx` x `ny` its size. The board's zones must carry their
// fills (refill first if they do not). A layer is a reference plane when its fills cover at least `min_coverage` of the board's bounding box.
ReferenceMap build_reference_map(const model::Board& b, const geom::Box& lat, Coord pitch, int nx, int ny, double min_coverage = 0.3);

// Rasterises a region (rings under the non-zero rule, holes included): 1 where the cell centre is inside. Exposed for the tests, which compare it with a
// point-in-region scan.
std::vector<std::uint8_t> rasterise(const geom::Rings& region, const geom::Box& lat, Coord pitch, int nx, int ny);

}  // namespace tmk::route
