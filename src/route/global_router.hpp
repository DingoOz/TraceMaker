// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Global routing on a coarse 3-D tile graph (design doc 05 §4 and §19, roadmap M6; CPU, the only path: §19 shows
// why there is no GPU version).
//
// The board is cut into square tiles about eight track pitches wide. Each tile edge on each copper layer gets a
// capacity: how many tracks fit across the narrowest cut between the two tile centres, measured with the exact
// obstacle model; each tile gets a via capacity (free via sites). Every two-pin connection is routed on the tile
// graph with A* (length + via cost + congestion), then negotiated congestion (PathFinder: McMurchie and Ebeling,
// FPGA 1995) rips up connections on overflowed edges or via tiles and re-routes them with growing history costs.
// Connections of one net share edges: an edge carries a net once however many of its connections use it, and a
// connection pays no congestion on an edge its net already holds, so a net's connections merge into a tree with
// shared trunks (the demand side of a Steiner topology). All costs are integers. The result is a corridor per
// connection: the tiles of its path on the layers it uses, widened by one tile, which the detailed router uses as
// soft guidance.
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
  int net = 0;                 // connections with the same net (> 0) share edges; 0 = shares with nothing
  geom::Box box_a, box_b;      // copper of the two end pads (cut lines: which side of a line an end is on)
};

struct GlobalOptions {
  Coord tile = 0;              // tile size; 0 = 8 x (narrowest width + clearance), clamped to 1.5..5 mm
  Coord pitch = 0;             // track pitch (width + clearance) used for capacities
  double via_cost_tiles = 2.0; // cost of a via, in tile lengths
  int iterations = 8;          // negotiation rounds after the first pass
  Coord via_diameter = 0, via_drill = 0;  // via used for the via capacity of a tile; 0 = no via capacity
  // Every pad of the nets being routed, as (net, copper box): a pad that straddles a cut line carries its net
  // across without a track, so that net is not counted there.
  std::vector<std::pair<int, geom::Box>> pads;
  Coord via_pitch = 0;         // spacing of the via sites sampled in a tile (via diameter + clearance)
};

// A straight line across the whole board and what has to cross it (escalation rung R6, doc 05 §20; the cut argument
// of Maley, "Single-Layer Wire Routing and Compaction", 1990, at the resolution of the capacity samples). Every net
// with a planned connection whose two end pads lie wholly on opposite sides, and no pad across the line, crosses it at
// least once, on some layer; `capacity` is an upper bound of the tracks
// that fit across it on all layers past the fixed copper. demand > capacity proves the placement unroutable.
struct CutLine {
  bool vertical = true;        // a line x = at (false: y = at)
  Coord at = 0;
  int capacity = 0, demand = 0;
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
  int overflow_via_tiles = 0;  // tiles with more planned vias than free via sites
  std::vector<CutLine> over_cuts;  // lines with demand > capacity, tightest line of each over-full tile boundary
  CutLine tightest;                // the line with the highest demand / capacity (capacity 0 counts as over-full)
  double seconds_capacity = 0, seconds_route = 0;  // set-up (obstacle sampling) and routing + negotiation
  int tile_of_x(Coord x) const { return static_cast<int>((x - origin.x) / tile); }
  int tile_of_y(Coord y) const { return static_cast<int>((y - origin.y) / tile); }
};

GlobalResult global_route(const Obstacles& obs, const geom::Box& bounds, int layers, const std::vector<GlobalNet>& nets,
                          const GlobalOptions& opt);

}  // namespace tmk::route
