// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Copper connectivity with KiCad's semantics, shared by the DRC (unconnected/dangling checks) and the router
// (which pads are already joined).
#include <numeric>
#include <vector>

#include "drc/copper.hpp"
#include "geom/poly_index.hpp"
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
// `overlap_links`: tracks, arcs, vias and board graphics of one net are also joined where their copper merely
// overlaps (KiCad 10 links connectivity items by shape collision, so a track crossing another of its net joins
// it). The DRC uses it; the router keeps end-point anchors (more conservative: it may route a redundant link).
Connectivity compute_connectivity(const model::Board& b, const CopperModel& cm, index::UniformGrid& grid, bool overlap_links = false);

// KiCad 10's item graph (pcbnew/connectivity, CN_VISITOR): items of any nets are linked when their copper touches
// on a common layer, except two items that cannot change net (pads, zone fills, free vias) on different nets.
// Tracks, arcs, vias and board copper graphics can change net. Footprint graphics and copper text are not part of it.
// Pads of one net keep the stricter pad-to-pad rule of compute_connectivity (centre inside the other pad, or one
// footprint's overlapping pads). Zone fills are left out of `adj`: testing every item against every large fill
// polygon is slow, and only a few questions need them, so those ask zones_touching.
struct ItemGraph {
  std::vector<std::vector<int>> adj;  // neighbours of each copper item, ascending (empty for items outside the graph)
};
bool in_item_graph(const CopperItem& it);
bool can_change_net(const CopperItem& it);
ItemGraph item_graph(const model::Board& b, const CopperModel& cm, index::UniformGrid& grid);
// Edge buckets of every zone fill (geom::PolygonIndex), so the zone questions below cost the nearby edges only.
struct ZoneFills {
  std::vector<int> slot;  // per copper item: index into `index`, -1 if the item is not a zone fill
  std::vector<geom::PolygonIndex> index;
  explicit ZoneFills(const CopperModel& cm);
};
// Zone-fill items on `layers` whose copper is closer than `clearance` to `s` (0, the default: overlap only, as
// KiCad's SHAPE::Collide; 1: touching counts),
// ascending item indices.
std::vector<int> zones_touching(const CopperModel& cm, const ZoneFills& zf, index::UniformGrid& grid, const geom::Shape& s,
                                model::LayerMask layers, Coord clearance = 0);

// The nets KiCad's DRC sees after loading a board (CN_CONNECTIVITY_ALGO::PropagateNets, run by every connectivity
// build): a via touching only zone fills takes a zone's net (its own if one of the zones has it, else the lowest);
// then in every cluster of the graph without zones whose pads carry one net only, tracks, arcs, vias and board
// graphics take that net (a stray via on a track joins the track's net; a track bridging two nets keeps its own).
// Clusters without pads take the net of their first item with a net. Returns one net per copper item.
std::vector<model::NetId> propagate_nets(const CopperModel& cm, const ItemGraph& g, const ZoneFills& zf, index::UniformGrid& grid);

}  // namespace tmk::drc
