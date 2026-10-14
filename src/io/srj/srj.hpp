// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Simple Route JSON (SRJ), tscircuit's autorouting problem format (design doc 08 §9, D90): read a problem, write
// it as a self-contained KiCad board plus project, and write a routed board's copper back as SRJ `traces`.
//
// Field reference: lib/types/srj-types.ts in github.com/tscircuit/tscircuit-autorouter (MIT) and the page
// "advanced/simple-route-json" on docs.tscircuit.com. SRJ is millimetres, y up, with layers named "top",
// "inner1" .. "innerN", "bottom"; KiCad is y down, so y is negated on the way in and back on the way out.
#include <optional>
#include <string>
#include <vector>

#include "core/units.hpp"
#include "model/board.hpp"

namespace tmk::io {

struct SrjPoint {
  double x = 0, y = 0;       // millimetres as written (kept so that export repeats the terminal exactly)
  std::vector<int> layers;   // stack indices, ascending; one entry for `layer`, several for `layers`
  std::string point_id, pcb_port_id;
};

struct SrjObstacle {
  std::string id, component;
  double x = 0, y = 0, width = 0, height = 0;  // centre and size, millimetres
  double rotation = 0;       // ccwRotationDegrees
  bool circle = false;       // shape "circle": diameter = width
  bool non_plated_hole = false, copper_pour = false;
  std::vector<int> layers;   // stack indices, ascending
  std::vector<std::string> connected_to;  // with offBoardConnectsTo appended: both join the obstacle to a net
  int net = -1;              // index into SrjProblem::nets, -1 = no net
};

struct SrjConnection {
  std::string name;
  std::optional<double> width;  // nominalTraceWidth (or the deprecated `width`), millimetres
  std::vector<SrjPoint> points;
  int net = -1;
};

// One electrical net: the connections and obstacles that tscircuit's connectivity map
// (lib/utils/getConnectivityMapFromSimpleRouteJson.ts) puts in one group.
struct SrjNet {
  std::string name;               // KiCad net name: the name of the net's first connection in file order
  std::vector<int> connections;   // ascending
  std::vector<int> obstacles;     // ascending
};

struct SrjRules {
  double min_trace_width = 0;
  std::optional<double> nominal_trace_width;
  std::optional<double> via_pad, via_hole;  // as resolved by tscircuit's getViaDimensions; unset = not in the file
  std::optional<double> obstacle_margin;    // defaultObstacleMargin
  std::optional<double> trace_to_pad, trace_to_hole, board_edge, via_to_pad, via_hole_to_via_hole, drill_to_drill, pad_to_pad;
  bool blind_buried_vias = false, via_in_pad = false;
};

// A bus whose `termination` is a plane: every connection of the bus ends in a plane on `layer`. Imported as a
// zone of the net on that layer covering the bounds (the balls then reach it with a via, as plane drops do).
struct SrjPlane {
  int net = -1;
  int layer = 0;
};

struct SrjProblem {
  int layer_count = 0;
  SrjRules rules;
  double min_x = 0, max_x = 0, min_y = 0, max_y = 0;  // bounds
  std::vector<std::pair<double, double>> outline;     // empty = the bounds rectangle is the board
  std::vector<SrjObstacle> obstacles;
  std::vector<SrjConnection> connections;
  std::vector<SrjNet> nets;
  std::vector<SrjPlane> planes;
  // Extension (not tscircuit's): `tracemakerTrackLayers`, the layers tracks may use; the others get a rule area that
  // forbids tracks and still lets vias through. Empty = every layer.
  std::vector<int> track_layers;
  std::vector<std::string> warnings;  // fields that were read but cannot be honoured
};

// Parses SRJ text. Throws std::runtime_error on malformed input or a field whose meaning is unknown where it
// matters (e.g. a layer name outside top/innerN/bottom).
SrjProblem read_srj(const std::string& text);
SrjProblem read_srj_file(const std::string& path);

// "top", "inner1", ..., "bottom" for a stack index, and back (-1 if the name is not a layer of the stack).
std::string srj_layer_name(int z, int layer_count);
int srj_layer_index(const std::string& name, int layer_count);

// tscircuit's fallbacks for what a problem leaves out: its router's default obstacle margin
// (`defaultObstacleMargin ?? 0.15`) and getViaDimensions' pad diameter.
inline constexpr double kSrjDefaultClearanceMm = 0.15;
inline constexpr double kSrjDefaultViaPadMm = 0.3;

struct SrjImportOptions {
  std::optional<Coord> clearance;  // copper clearance to use instead of defaultObstacleMargin / the 0.15 mm fallback
  // Drill edge to drill edge, instead of the problem's minViaHoleEdgeToViaHoleEdgeClearance /
  // minPlatedHoleDrillEdgeToDrillEdgeClearance (no rule when it has neither).
  std::optional<Coord> hole_to_hole;
  // The board outline (and plane zones) grow by this on every side; pads keep their coordinates. For problems whose
  // connections end exactly on the bounds (breakout points), where the board-edge rule would forbid the copper.
  Coord outline_margin = 0;
  // Obstacles of a net that no connection point lies on are keep-outs, not pads of the net: a fan-out problem lists
  // the far component's pads in the nets (connectedTo) but only asks for the connections at the package.
  bool unpointed_keepout = false;
};

struct SrjBoardFiles {
  std::string pcb, pro, dru;  // file contents; `dru` is empty when no custom rule is needed
  std::vector<std::string> warnings;
  int nets = 0, pads = 0, terminal_pads = 0;  // terminal_pads: pads made for points that lie on no obstacle
  // Connections the problem asks for (points - 1 per connection) and the ratsnest size the board implies
  // (pads - 1 per net). They differ when a net has a pad without a point or its connections do not form a tree.
  int srj_pairs = 0, board_pairs = 0;
  Coord clearance = 0, hole_to_hole = 0, track_width = 0, via_diameter = 0, via_drill = 0;
};

// The board is the problem mirrored in y at the same coordinates (x, -y), one footprint per componentId. The
// output depends only on the problem and the options (UUIDs are counted, not drawn), so it is byte-identical
// across runs.
SrjBoardFiles srj_to_kicad(const SrjProblem& problem, const SrjImportOptions& opt = {});

struct SrjExport {
  std::string json;                   // the problem with `traces` set to the routed copper
  int connections = 0, routed = 0;
  std::vector<std::string> unrouted;  // connection names without a copper path between their points
  int traces = 0, extra_traces = 0;   // extra: copper on no path between two points of a connection (stubs, loops)
  int vias = 0;
  std::vector<std::string> warnings;
};

// Writes the tracks and vias of `routed` (a board made by srj_to_kicad from `problem_text`, then routed) as SRJ
// traces: one trace per pair of consecutive points of a connection, following the copper between their pads,
// then the copper no such path used. Nets are matched by name, pads by position.
SrjExport srj_export(const std::string& problem_text, const model::Board& routed);

}  // namespace tmk::io
