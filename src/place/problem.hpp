// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Placement problem extracted from a board (design doc 04 §2): parts with courtyards, holes and pins per
// rotation, weighted nets, the board outline, keepouts and spacing rules. Everything is in integer nm; a part's
// geometry is stored as offsets from its footprint origin for each of the four 90° rotations.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "geom/shape.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::place {

using geom::Box;
using geom::Point;
using geom::Shape;

inline std::size_t z(int i) { return static_cast<std::size_t>(i); }

// Net weights are small integers so every cost is an exact integer (CLAUDE.md rule 2).
inline constexpr int kSignalWeight = 10;
inline constexpr int kPowerWeight = 1;
inline constexpr Coord kEdgeTolerance = 250'000;  // courtyard may extend this far past the board edge

// A piece of copper with what KiCad's clearance check needs: layers, net and the clearance it asks for
// (its net class, local overrides and board minimum; pair clearance = max of the two).
struct CopperShape {
  Shape s;
  model::LayerMask layers = 0;
  model::NetId net = 0;         // board net id (0 = no net)
  Coord need = 0;
};

struct PartGeom {               // one rotation of a part, offsets from the footprint origin
  std::array<std::vector<Shape>, 2> cy;  // courtyard polygons per side (empty = no courtyard on that side)
  std::vector<Shape> through;   // drilled holes and plated-through pad copper (obstacles on both sides)
  std::vector<Shape> pads;      // pad copper (for the copper-to-edge clearance)
  std::vector<CopperShape> copper;  // pads, footprint copper graphics/text, NPTH holes (copper clearance)
  Box copper_box;               // bounding box of `copper`
  Box body;                     // bounding box of courtyards, through obstacles and copper
  // Courtyards shrunk by kEdgeTolerance: what must stay inside the board outline. KiCad itself only checks pad
  // copper against the edge (copper_edge_clearance); a courtyard may reach a little past it.
  std::array<std::vector<Shape>, 2> cy_in;
  Box edge_box;                 // bounding box of the inset courtyards and pads inflated by the edge clearance
};

// Orientation state of a part (Placement::rot): bits 0–1 = quarter turns r, bit 2 = flipped to the other side.
// State r means angle0 + 90 r on the input side; state 4 + r means the KiCad flip (top/bottom about the origin:
// offsets mirrored in y, orientation −angle0) followed by r quarter turns, i.e. −angle0 + 90 r on the other side.
inline constexpr int kStates = 8;
inline constexpr std::uint8_t kFlipBit = 4;
inline bool flipped(int state) { return (state & kFlipBit) != 0; }
// State with the same side as `state` and `r` quarter turns.
inline std::uint8_t with_turn(int state, int r) { return static_cast<std::uint8_t>((state & kFlipBit) | (r & 3)); }
inline Point mirror_y(Point p) { return Point{p.x, -p.y}; }
// Exchanging two interchangeable parts a and b (same footprint and input side; angle0_b − angle0_a = 90° dk): the
// state that gives a the absolute pose b has in `b_state`. Flipping negates angle0, so dk changes sign there.
// (b takes a's pose with swap_state(a_state, −dk).)
inline std::uint8_t swap_state(int b_state, int dk) { return with_turn(b_state, (b_state & 3) + (flipped(b_state) ? -dk : dk) + 4); }

struct Part {
  int fp = -1;                  // footprint index in the board
  std::string ref, lib_id;
  bool movable = false;
  std::string fixed_reason;     // why a part is fixed (locked, mounting hole, ...)
  int side = 0;                 // 0 front, 1 back: the footprint's side in the input
  // May move to the other side (states 4–7): movable, surface mount only (no holes), the board has two copper
  // sides, flipping was asked for, and the writer can mirror the footprint (doc 04 §3 C/E, D48).
  bool flippable = false;
  std::string flip_reason;      // why a movable part may not flip (empty when it may, or when flipping is off)
  Point pos0;                   // original origin
  double angle0 = 0;            // original absolute orientation (degrees); see kStates for state angles
  std::array<PartGeom, kStates> geom;  // per state (states 4–7 only filled for flippable parts)
  bool may_flip() const { return movable && flippable; }  // movability can be revoked after extraction
  int side_in(int state) const { return side ^ (flipped(state) ? 1 : 0); }
  double angle_of(int state) const { return flipped(state) ? -angle0 + 90.0 * (state & 3) : angle0 + 90.0 * (state & 3); }
  std::vector<int> pins;        // indices into Problem::pins
  Coord area = 0;               // courtyard box area incl. clearance (nm², saturating), for spreading
  std::uint64_t shape_key = 0;  // equal keys = interchangeable footprints (swap moves)
};

struct Pin {
  int part = -1;
  int net = -1;                 // index into Problem::nets
  std::array<Point, kStates> off;  // offset from the part origin per state
  bool one_side = false;        // a surface-mount pad: on the part's side only (via estimate when sides differ)
};

struct PNet {
  std::string name;
  int weight = kSignalWeight;
  bool signal = true;           // counted for airwire crossings
  // Design-intent pseudo-net (decoupling capacitor to its IC's supply pin): part of the objective only, never
  // reported as wirelength, never routed, never counted for congestion.
  bool affinity = false;
  std::vector<int> pins;
  // Fixed coordinates inside the net's bounding box (an edge pull, doc 15 CONN-01): a one-pin net with an x anchor
  // has HPWL |pin.x - ax|, the distance to a vertical board edge. Affinity nets only.
  bool has_ax = false, has_ay = false;
  Coord ax = 0, ay = 0;
};

struct Keepout {
  Shape poly;
  bool side[2] = {false, false};
};

struct Problem {
  std::vector<Part> parts;
  std::vector<Pin> pins;
  std::vector<PNet> nets;
  std::vector<Point> outline;                // largest Edge.Cuts loop (empty if none could be assembled)
  std::vector<std::vector<Point>> cutouts;   // other closed Edge.Cuts loops
  std::vector<Shape> edges;                  // every Edge.Cuts piece as segments (r = 0)
  std::vector<Keepout> keepouts;
  std::vector<CopperShape> fixed_copper;     // board copper that never moves: tracks, vias, copper graphics and text
  Coord max_need = 0;                        // largest copper clearance any item asks for
  int copper_layers = 2;
  Coord clearance = 250'000;                 // courtyard-to-courtyard clearance
  bool clearance_is_default = true;          // not from a board rule or the command line
  Coord edge_clearance = 0;                  // pad copper to board edge
  Box region;                                // where parts may go (outline bbox)
  std::vector<std::string> notes;            // extraction decisions worth reporting

  int movable_count() const;
  int flippable_count() const;
};

// A design-intent pseudo-net between two board pads (doc 15 P1): `pad_a` belongs to the part that should sit
// near `pad_b` (an ESD part near its connector, a crystal near its IC's oscillator pins, ...).
struct PadAffinity {
  int pad_a = -1, pad_b = -1;       // board pad indices
  int weight = kSignalWeight;
  std::string name;                 // "~XTAL-01 Y1-U1"
};

// An objective-only pull of a part toward the board outline (doc 15 CONN-01, opt-in): the body point nearest to the
// outline in the input becomes a one-pin pseudo-net anchored on the nearest outline segment's axis (x for a mostly
// vertical segment, y for a mostly horizontal one), so the pull is exact in the annealer's integer cost and
// rotating the part away from the edge costs too.
struct EdgePull {
  int footprint = -1;
  std::vector<Point> body;          // absolute candidate points (courtyard outline, else pad corners)
  int weight = 2 * kSignalWeight;
  std::string name;                 // "~CONN-01 J3 edge"
};

struct ExtractOptions {
  bool fix_edge_connectors = true;  // connectors (J*, P*, CN*, USB*) touching the outline stay put
  Coord courtyard_clearance = -1;   // override (-1: from the rules, else default_clearance)
  Coord default_clearance = 250'000;  // used when the board has no courtyard rule (KiCad's own default is 0)
  bool decap_affinity = true;       // tie each decoupling capacitor to the nearest supply pin of its IC
  int decap_weight = kSignalWeight; // weight of those ties (D25: signal weight)
  int crules_weight_pct = 100;      // scale of the component-rule proximity weights (100 = as the rules say)
  // Full mode with component rules: place with the decoupling ties only, then lock the tied capacitors and refine
  // with the component-rule pulls (they compete for the space next to the IC otherwise; doc 15 §14.3).
  bool crules_two_stage = true;
  // Extra objective-only pseudo-nets from component rules (crules::placement_affinities). Parts already tied by
  // decap_affinity are skipped.
  std::vector<PadAffinity> affinities;
  // Edge pulls for movable parts (crules::edge_attractions); fixed parts are skipped.
  std::vector<EdgePull> edge_pulls;
  // Side assignment (doc 04 §3 C/E, D48): movable surface-mount parts may move to the other side. `flip_ok` (one
  // entry per footprint; empty = all) says whether the writer can mirror the footprint, `flip_why` why not;
  // `keep_side` lists references the user pins to their side.
  bool flip = false;
  std::vector<std::uint8_t> flip_ok;
  std::vector<std::string> flip_why;
  std::vector<std::string> keep_side;
};

// Builds the problem. `rules` and `board_path` give the courtyard clearance (custom rules or .kicad_pro).
Problem extract(const model::Board& b, const model::DesignRules& rules, const std::string& board_path,
                const ExtractOptions& opt = {});

// Geometry helpers.
Shape translated(const Shape& s, Point d);
// Mirror in y about the origin (polygon winding kept).
Shape mirrored(const Shape& s);
// Copper layer mask of the other side: bit i <-> bit n−1−i for n copper layers (KiCad FlipLayerMask).
model::LayerMask flip_layers(model::LayerMask m, int n);
Point rot90(Point p, int r);       // KiCad rotation by r * 90 degrees
std::vector<Point> convex_hull(std::vector<Point> pts);
// Inner parallel polygon of a convex polygon at distance t; a point at the centroid if it is thinner than 2t.
Shape inset_convex(const Shape& s, Coord t);
bool power_like_name(const std::string& name);
bool ground_like_name(const std::string& name);

}  // namespace tmk::place
