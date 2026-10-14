// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Reference-return audit of high-speed nets (issue #7, docs/05-routing.md §42, D114).
//
// KiCad's DRC cannot tell whether a fast trace has continuous copper under it. A dogbone escape punches antipads through the reference
// pour, a plane can be split, a layer change can come without a nearby stitch: the signal is DRC-clean and its return current still
// walks around the void, which grows the loop area and the inductance. This pass answers a geometric question per net: does the trace
// have copper under it on the adjacent reference layer, and where it does not, how far does the return detour?
//
// It is read-only (it adds nothing to the board) and it is a geometric model, not a field solver: the return of a signal over copper
// is taken to be its mirror image on the reference layer, across a void it is the shortest path that stays on the remaining reference
// copper (Dijkstra over the visibility graph of the reference polygon's vertices; Lee and Preparata 1984), and at a layer change it
// closes through the nearest stitching via of the reference net, or the nearest capacitor between the two reference nets, within a
// radius. The return does not close through "one assumed GND ball": at a footprint the nearest pad of the reference net is the
// assumed return pad (the comment on the issue).
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "geom/point.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::analysis {

struct ReturnPathOptions {
  // Which nets: a signal is analysed if it matches any selector (default: none, the pass is off). Name patterns are KiCad wildcards.
  std::vector<std::string> name_patterns;
  std::vector<std::string> net_classes;
  std::vector<std::pair<model::NetId, model::NetId>> pair_nets;  // differential pairs, resolved by the caller (route::named_pairs)
  bool diff_pairs = false;                                       // analyse the halves of `pair_nets`
  // "PATTERN=LAYER": the reference layer of the nets that match, instead of the adjacent layer with the most copper under the trace.
  std::vector<std::pair<std::string, std::string>> reference_layers;
  double stitch_radius_mm = 5.0;   // how far a stitching via or a capacitor may be from a layer change
  double sample_mm = 0.05;         // spacing of the samples along a trace
  double min_void_mm = 0.1;        // a stretch without copper shorter than this is a sampling artefact, not a void
  double warn_detour_mm = 2.0;     // thresholds: warnings, not DRC errors
  double warn_separation_mm = 1.0;
  bool refill = true;              // zones without fills are refilled first (the engine's refill, not KiCad's)
  bool refill_always = false;      // refill every zone: the fills in the file predate copper that was just routed
};

struct ReturnVoid {
  int layer = 0;                       // signal layer (copper index)
  geom::Point entry, exit;             // where the trace leaves and regains copper under it
  double signal_mm = 0;                // the trace between them
  double return_mm = 0;                // the return path between them (the shortest path on the reference copper)
  double detour_mm = 0;                // return - signal, at least 0
  double max_separation_mm = 0;        // the farthest the return gets from the trace
  geom::Point farthest;                // where
  bool no_path = false;                // the reference copper does not connect the two ends (a split plane): no return on this layer
  std::vector<geom::Point> path;       // the return path, entry to exit
};

struct ReturnLayerChange {
  geom::Point at;                      // the signal via
  int from_layer = 0, to_layer = 0;
  std::string ref_from, ref_to;        // reference net names ("" = no reference copper under the via)
  std::string closure;                 // "stitch via", "capacitor", "none", "no reference"
  double distance_mm = -1;             // to the closure
  geom::Point closure_at;
  bool missing = false;
};

struct ReturnEnd {                     // a trace end at a pad: the assumed return pad
  geom::Point at;
  std::string pad;                     // "REF.NUM"
  std::string ref_pad;                 // nearest pad of the reference net, "" = none within the radius
  double distance_mm = -1;
  geom::Point ref_at;
};

struct NetReturn {
  model::NetId net = 0;
  std::string name;
  std::vector<int> layers;             // signal layers
  std::vector<int> reference_layers;   // the reference layer of each signal layer, same order (-1 = none)
  double signal_mm = 0;
  double detour_mm = 0;                // summed over the voids
  double max_separation_mm = 0;
  int void_count = 0;
  int split_count = 0;                 // voids with no return path
  int missing_stitches = 0;
  double uncovered_mm = 0;             // trace length with no reference layer at all
  std::vector<ReturnVoid> voids;
  std::vector<ReturnLayerChange> changes;
  std::vector<ReturnEnd> ends;
  // For the viewer: the signal as polylines per layer, and the return along it (mirror over copper, the detour across a void).
  std::vector<std::pair<int, std::vector<geom::Point>>> signal;
  std::vector<std::pair<int, std::vector<geom::Point>>> ret;
  std::vector<std::string> warnings;
};

struct ReturnPathReport {
  std::vector<NetReturn> nets;
  std::vector<std::string> notes;      // what the run could not do (arcs skipped, zones refilled, a net without a reference ...)
  ReturnPathOptions options;
};

// Analyses the nets the options select on `b` as it stands (routed copper included). Deterministic.
ReturnPathReport analyse_return_paths(const model::Board& b, const model::DesignRules& rules, const ReturnPathOptions& opt);

// The report as JSON (nanometres converted to millimetres), and one `return_path` viewer message per net (docs/13-viewer-protocol.md).
nlohmann::json return_path_json(const model::Board& b, const ReturnPathReport& r);
std::string return_path_message(const model::Board& b, const NetReturn& n);
// A one-line text form per net and the lines under it that explain a warning.
std::vector<std::string> return_path_lines(const model::Board& b, const ReturnPathReport& r);

}  // namespace tmk::analysis
