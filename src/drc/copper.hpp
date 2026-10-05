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
};

// Pad copper shape in absolute coordinates.
std::vector<geom::Shape> pad_shapes(const model::Pad& pad);

CopperModel build_copper(const model::Board& b);

}  // namespace tmk::drc
