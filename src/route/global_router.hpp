// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Global routing on a coarse 3-D tile graph (design doc 05 §4, roadmap M6; CPU reference path).
//
// The board is cut into square tiles about eight track pitches wide. Each tile edge on each copper layer gets a
// capacity: how many tracks fit through the free part of the shared boundary, measured with the exact obstacle
// model. Every two-pin connection is routed on the tile graph with A* (length + via cost + congestion), then
// negotiated congestion (PathFinder: McMurchie and Ebeling, FPGA 1995) rips up connections on overflowed edges and
// re-routes them with growing history costs. The result is a corridor per connection: the tiles of its path on
// each layer, widened by one tile, which the detailed router uses as soft guidance.
#include <array>
#include <cstdint>
#include <vector>

#include "core/units.hpp"
#include "geom/point.hpp"
#include "model/board.hpp"

namespace tmk::route {

class Obstacles;

struct GlobalNet {             // one two-pin connection
  geom::Point a, b;
  model::LayerMask layers_a = 0, layers_b = 0;  // copper layers each end can start on
  Coord half_width = 0;
};

struct GlobalOptions {
  Coord tile = 0;              // tile size; 0 = 8 x (narrowest width + clearance), clamped to 1.5..5 mm
  Coord pitch = 0;             // track pitch (width + clearance) used for capacities
  double via_cost_tiles = 2.0; // cost of a via, in tile lengths
  int iterations = 8;          // negotiation rounds after the first pass
};

struct GlobalResult {
  int tiles_x = 0, tiles_y = 0, layers = 0;
  Coord tile = 0;
  geom::Point origin;
  // corridor[k][(l * tiles_y + ty) * tiles_x + tx] != 0: tile on layer l is in connection k's corridor.
  std::vector<std::vector<std::uint8_t>> corridor;
  // Bounding box of each corridor in tiles (x0, y0, x1, y1, inclusive); the detailed router's confined window.
  std::vector<std::array<int, 4>> corridor_box;
  std::vector<int> vias;       // global vias per connection
  // util[(l * tiles_y + ty) * tiles_x + tx]: planned tracks through the tile's four boundaries on layer l, in
  // eighths of their capacity (8 = full, capped at 255). The detailed router's congestion map.
  std::vector<std::uint8_t> util;
  int overflow_edges = 0;      // edges still over capacity after negotiation
  long total_overflow = 0;
  int tile_of_x(Coord x) const { return static_cast<int>((x - origin.x) / tile); }
  int tile_of_y(Coord y) const { return static_cast<int>((y - origin.y) / tile); }
};

GlobalResult global_route(const Obstacles& obs, const geom::Box& bounds, int layers, const std::vector<GlobalNet>& nets,
                          const GlobalOptions& opt);

}  // namespace tmk::route
