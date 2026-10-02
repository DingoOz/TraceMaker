#pragma once
// Copper connectivity with KiCad's semantics, shared by the DRC (unconnected/dangling checks) and the router
// (which pads are already joined).
#include <numeric>
#include <vector>

#include "drc/copper.hpp"
#include "index/uniform_grid.hpp"
#include "model/board.hpp"

namespace tmk::drc {

struct UnionFind {
  std::vector<int> p;
  explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) {
    while (p[static_cast<std::size_t>(x)] != x) x = p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
    return x;
  }
  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) p[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
  }
};

struct Connectivity {
  std::vector<int> root;                    // cluster root per copper item
  std::vector<std::uint8_t> end_hit;        // per track/arc item: [2i] start connected, [2i+1] end connected
  std::vector<model::LayerMask> via_layers; // per via item: layers with a connection
};

// `grid` must contain every item of `cm` (ids = item indices).
Connectivity compute_connectivity(const model::Board& b, const CopperModel& cm, index::UniformGrid& grid);

}  // namespace tmk::drc
