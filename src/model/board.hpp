#pragma once
// The board model (design doc 02 §3): every object TraceMaker reads from a .kicad_pcb, in absolute
// integer-nanometre coordinates, with a link back to its s-expression node for lossless writing.
#include <climits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/units.hpp"
#include "geom/point.hpp"
#include "model/stackup.hpp"
#include "sexpr/sexpr.hpp"

namespace tmk::model {

using geom::Box;
using geom::Point;
using NetId = std::int32_t;  // 0 = no net
using LayerMask = std::uint64_t;  // bit i = copper layer i in stack order (0 = F.Cu, last = B.Cu)

inline constexpr LayerMask layer_bit(int copper_index) { return LayerMask{1} << copper_index; }

struct LayerDef {
  int ordinal = 0;           // KiCad layer id as written in the file
  std::string name;          // canonical name, e.g. "F.Cu", "In1.Cu", "Edge.Cuts"
  std::string file_name;     // name as written in this file's layer table (old boards may use "Front")
  std::string type;          // signal, power, mixed, jumper, user
  std::string user_name;     // optional user-visible name
  int copper_index = -1;     // stack index for copper layers, -1 otherwise
};

struct Net {
  NetId id = 0;
  std::string name;
  int file_number = -1;      // number in a KiCad <= 9 net table, -1 for KiCad 10 files
};

enum class PadType : std::uint8_t { Smd, ThruHole, NpThruHole, Connect };
enum class PadShape : std::uint8_t { Circle, Rect, Oval, Trapezoid, RoundRect, ChamferedRect, Custom };

struct Pad {
  int footprint = -1;
  std::string number;
  PadType type = PadType::Smd;
  PadShape shape = PadShape::Rect;
  Point pos;                 // absolute centre
  double angle = 0;          // absolute orientation, degrees
  Coord size_x = 0, size_y = 0;
  Coord drill_x = 0, drill_y = 0;  // 0 = no hole
  bool drill_oval = false;
  Point drill_offset;        // copper shape offset from the hole (pad position), in the pad's own frame
  LayerMask copper = 0;      // copper layers the pad has copper on
  std::vector<std::string> layers;  // all layer names as written (incl. mask/paste, wildcards expanded)
  double roundrect_ratio = 0;
  double chamfer_ratio = 0;
  std::uint8_t chamfer_corners = 0;  // bit 0 TL, 1 TR, 2 BL, 3 BR
  Coord trapezoid_dx = 0, trapezoid_dy = 0;  // rect_delta
  std::vector<std::vector<Point>> custom_polys;  // custom primitives (gr_poly) in the pad frame
  Coord clearance = -1;      // local override, -1 = none
  Coord mask_margin = INT64_MIN;  // local solder-mask expansion (unset = INT64_MIN)
  // Schematic pin name and electrical type (KiCad 6+ boards; empty in older files). Read-only, used to
  // recognise component roles (doc 15 §3.1).
  std::string pinfunction, pintype;
  NetId net = 0;
  sexpr::NodeId node = sexpr::kNoNode;
};

struct Graphic {
  enum class Kind : std::uint8_t { Line, Arc, Circle, Rect, Poly, Curve } kind = Kind::Line;
  std::string layer;
  Point a, b, c;             // line: a→b; arc: start a, mid c, end b; circle: centre a, point b; rect: a,b corners
  std::vector<Point> pts;    // poly / curve control points
  Coord width = 0;
  bool filled = false;
  NetId net = 0;             // KiCad 8+ copper graphics can belong to a net
  int footprint = -1;        // owning footprint, -1 for board graphics
  sexpr::NodeId node = sexpr::kNoNode;
};

struct Text {                // copper text is an obstacle; only position, layer and rough extent are kept
  std::string layer;
  std::string text;
  Point pos;
  double angle = 0;
  Coord height = 0, width = 0, thickness = 0;
  int justify_h = 0;         // -1 left, 0 centre, 1 right
  int justify_v = 0;         // -1 top, 0 centre, 1 bottom
  bool mirror = false;
  int footprint = -1;
  bool hidden = false;
};

struct Footprint {
  std::string lib_id;
  std::string reference, value;
  std::string description, keywords;  // footprint (descr ...)/(tags ...) or Description property; read-only
  Point pos;
  double angle = 0;
  bool back = false;
  bool locked = false;
  bool attr_smd = false, attr_through_hole = false, board_only = false, exclude_from_pos = false,
       exclude_from_bom = false, dnp = false, allow_missing_courtyard = false;
  std::vector<int> pads;
  std::vector<int> graphics;
  Coord clearance = -1;      // footprint-level clearance override, -1 = none
  Coord mask_margin = INT64_MIN;  // footprint-level solder-mask expansion (unset = INT64_MIN)
  std::vector<std::vector<std::string>> net_tie_groups;  // pad numbers joined by the footprint's own copper
  std::string uuid;
  sexpr::NodeId node = sexpr::kNoNode;
};

struct Track {
  Point a, b;
  Coord width = 0;
  int layer = -1;            // copper index
  NetId net = 0;
  bool locked = false;
  sexpr::NodeId node = sexpr::kNoNode;
};

struct ArcTrack {
  Point a, mid, b;
  Coord width = 0;
  int layer = -1;
  NetId net = 0;
  bool locked = false;
  sexpr::NodeId node = sexpr::kNoNode;
};

enum class ViaType : std::uint8_t { Through, Blind, Micro };

struct Via {
  Point pos;
  Coord size = 0, drill = 0;
  int layer_top = 0, layer_bottom = 0;  // copper indices, top <= bottom
  ViaType type = ViaType::Through;
  NetId net = 0;
  bool locked = false;
  sexpr::NodeId node = sexpr::kNoNode;
};

struct Zone {
  NetId net = 0;
  LayerMask copper = 0;
  std::vector<std::string> layers;
  std::string name;
  int priority = 0;
  bool rule_area = false;
  bool keepout_tracks = false, keepout_vias = false, keepout_pads = false, keepout_pour = false,
       keepout_footprints = false;
  std::vector<std::vector<Point>> outline;  // main outline + holes (arcs flattened)
  std::vector<std::pair<int, std::vector<Point>>> fills;  // (copper index, filled polygon)
  int footprint = -1;
  sexpr::NodeId node = sexpr::kNoNode;
};

struct Board {
  std::int64_t version = 0;
  std::string generator;
  std::vector<LayerDef> layers;
  std::vector<int> copper;   // indices into `layers`, in stack order
  std::vector<Net> nets;     // nets[0] is the unnamed net
  bool named_nets = false;   // true for KiCad 10 "(net "name")" syntax
  std::vector<Footprint> footprints;
  std::vector<Pad> pads;
  std::vector<Graphic> graphics;
  std::vector<Text> texts;
  std::vector<Track> tracks;
  std::vector<ArcTrack> arcs;
  std::vector<Via> vias;
  std::vector<Zone> zones;
  Coord thickness = 1'600'000;
  Coord pad_to_mask_clearance = 0;  // board solder-mask expansion for pads (setup)
  bool vias_tented = false;         // (setup (tenting front back)); old boards: untented
  Stackup stackup;                  // (setup (stackup ...)); present == false when the file has none
  std::vector<std::string> warnings;

  int copper_count() const { return static_cast<int>(copper.size()); }
  const std::string& copper_name(int idx) const { return layers[static_cast<std::size_t>(copper[static_cast<std::size_t>(idx)])].name; }
  const std::string& copper_file_name(int idx) const { return layers[static_cast<std::size_t>(copper[static_cast<std::size_t>(idx)])].file_name; }
  int copper_index(std::string_view name) const;   // -1 if not copper (canonical or file name)
  NetId net_by_name(std::string_view name) const;  // 0 if unknown
  // Bounding box of everything on Edge.Cuts (graphics stroke widths excluded).
  Box edge_bbox() const;

  std::unordered_map<std::string, NetId> net_index;
};

}  // namespace tmk::model
