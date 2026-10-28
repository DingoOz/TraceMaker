// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Copper items with exact shapes, built from the board model, for DRC and routing obstacles.
#include <vector>

#include "geom/shape.hpp"
#include "model/board.hpp"

namespace tmk::drc {

enum class ItemKind : std::uint8_t { Pad, Track, Arc, Via, Zone, Graphic };
const char* kind_name(ItemKind k);  // KiCad expression type names: Pad, Track, Arc, Via, Zone, Graphic

struct CopperItem {
  ItemKind kind = ItemKind::Track;
  int index = -1;                 // index in the board's pads/tracks/arcs/vias/zones/graphics vector
  int sub = 0;                    // zone fill polygon index
  model::NetId net = 0;
  model::LayerMask layers = 0;    // copper layers the shape is on
  std::vector<geom::Shape> shapes;  // copper shape (identical on every layer in `layers`)
  geom::Box box;
  int footprint = -1;
  model::Point pos;               // reporting position
  Coord width = 0;                // tracks/arcs: width; vias: diameter
  int owner = -1;                 // router connection that created this item (-1 = fixed copper)
  bool removed = false;           // ripped up (router working model only)
  bool free_via = false;          // via marked (free yes): its net is fixed for KiCad's net propagation
  // Vias and pads whose copper KiCad removes on layers nothing connects to (model::UnconnectedLayers, doc 05 §60). On those
  // layers only the hole is left, and KiCad's clearance test takes the hole for the item's copper there. Such an item is split:
  // the item with the copper (`layers` without the bare layers) and a second item of the same kind and index whose layers are
  // the bare ones and whose shape is the hole. `bare_part` on the first points to the second, `bare_of` on the second to the
  // first. The second is no node of the connectivity graph and is not reported on its own. `span` is the padstack's whole layer
  // set (KiCad's GetLayerSet), which rule conditions see; 0 for items that are not split.
  model::LayerMask span = 0;
  int bare_part = -1, bare_of = -1;
};

struct Hole {
  geom::Shape shape;
  int item = -1;                  // copper item owning the hole (-1 for NPTH pads without copper)
  int pad = -1, via = -1;
  bool plated = true;
  model::NetId net = 0;
  model::Point pos;
  Coord clearance = -1;         // local clearance of the owning pad/footprint (applies as hole clearance), -1 = none
  bool removed = false;
};

struct CopperModel {
  std::vector<CopperItem> items;
  std::vector<Hole> holes;
  std::vector<geom::Shape> edges;  // Edge.Cuts outline pieces (open polylines, r = 0)
  // Margin-layer graphics: KiCad's copper_edge_clearance treats them like the board edge
  // (drc_test_provider_edge_clearance collects Edge.Cuts and Margin), but they are no part of the outline.
  std::vector<geom::Shape> margins;
};

// Pad copper shape in absolute coordinates.
std::vector<geom::Shape> pad_shapes(const model::Pad& pad);

CopperModel build_copper(const model::Board& b);

// KiCad's ConditionallyFlashed: the layers of a via, or of a plated through-hole pad on a board of `copper_count` layers, whose
// copper depends on what connects there.
model::LayerMask conditional_layers(const model::Via& v);
model::LayerMask conditional_layers(const model::Pad& p, int copper_count);

// The copper layers of each via (`vias`) and pad (`pads`) as KiCad 10.0.6 flashes them (PCB_VIA::FlashLayer, PAD::FlashLayer): a
// layer keeps its copper unless the padstack removes unconnected copper there and neither a zone connection (zone_layers) nor a
// track, arc, via or pad of the net (not of the pad's own footprint) touches it. Touching is CN_VISITOR's test: on such a layer an
// item is only its hole. Items that remove nothing keep their whole span.
struct Flashing {
  std::vector<model::LayerMask> vias, pads;
};
Flashing flash_layers(const model::Board& b);

// Closed loops chained from Edge.Cuts pieces whose ends meet within `tol`; pieces that close no loop are dropped
// (`all_closed`, if given, tells whether any was).
std::vector<std::vector<model::Point>> edge_loops(const std::vector<geom::Shape>& edges, Coord tol = 2'000, bool* all_closed = nullptr);

}  // namespace tmk::drc
