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

struct Part {
  int fp = -1;                  // footprint index in the board
  std::string ref, lib_id;
  bool movable = false;
  std::string fixed_reason;     // why a part is fixed (locked, mounting hole, ...)
  int side = 0;                 // 0 front, 1 back (the footprint's side; never changed)
  Point pos0;                   // original origin
  double angle0 = 0;            // original absolute orientation (degrees); rotation r means angle0 + 90 r
  std::array<PartGeom, 4> geom; // per rotation r = 0..3
  std::vector<int> pins;        // indices into Problem::pins
  Coord area = 0;               // courtyard box area incl. clearance (nm², saturating), for spreading
  std::uint64_t shape_key = 0;  // equal keys = interchangeable footprints (swap moves)
};

struct Pin {
  int part = -1;
  int net = -1;                 // index into Problem::nets
  std::array<Point, 4> off;     // offset from the part origin per rotation
};

struct PNet {
  std::string name;
  int weight = kSignalWeight;
  bool signal = true;           // counted for airwire crossings
  // Design-intent pseudo-net (decoupling capacitor to its IC's supply pin): part of the objective only, never
  // reported as wirelength, never routed, never counted for congestion.
  bool affinity = false;
  std::vector<int> pins;
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
};

struct ExtractOptions {
  bool fix_edge_connectors = true;  // connectors (J*, P*, CN*, USB*) touching the outline stay put
  Coord courtyard_clearance = -1;   // override (-1: from the rules, else default_clearance)
  Coord default_clearance = 250'000;  // used when the board has no courtyard rule (KiCad's own default is 0)
  bool decap_affinity = true;       // tie each decoupling capacitor to the nearest supply pin of its IC
};

// Builds the problem. `rules` and `board_path` give the courtyard clearance (custom rules or .kicad_pro).
Problem extract(const model::Board& b, const model::DesignRules& rules, const std::string& board_path,
                const ExtractOptions& opt = {});

// Geometry helpers.
Shape translated(const Shape& s, Point d);
Point rot90(Point p, int r);       // KiCad rotation by r * 90 degrees
std::vector<Point> convex_hull(std::vector<Point> pts);
// Inner parallel polygon of a convex polygon at distance t; a point at the centroid if it is thinner than 2t.
Shape inset_convex(const Shape& s, Coord t);
bool power_like_name(const std::string& name);
bool ground_like_name(const std::string& name);

}  // namespace tmk::place
