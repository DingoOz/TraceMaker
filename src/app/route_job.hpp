// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One `tracemaker route` run as a reusable function, shared by the CLI (src/app/main.cpp) and the Python bindings
// (bindings/), so both take exactly the same code path (design doc 08 §5).
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "route/bus.hpp"
#include "route/router.hpp"

namespace tmk::app {

struct RouteJob {
  std::string in;                 // input .kicad_pcb (its project files supply the design rules)
  std::string out;                // output .kicad_pcb; empty = do not write a board
  route::RouterOptions opt;       // `sink` is set here when viewing or recording
  int threads = 8;                // worker threads for the portfolio variants
  // Portfolio size (1 = single router). 0 = automatic: all variants (route::portfolio_size()) with a work budget,
  // so the output does not depend on `threads` (D47); otherwise `threads` (wall-clock mode: one variant per thread).
  int variants = 0;
  std::string kb_path;            // knowledge base file; empty = none
  std::string items_out;          // --emit-items file; empty = none
  std::string json_out;           // --json summary file; empty = none
  bool view = false;              // stream to the browser viewer
  std::string view_host = "0.0.0.0";
  int view_port = 8766;
  bool hold = false;              // keep serving the viewer after routing; never returns (CLI only)
  std::string record;             // events file for replay (ignored while viewing)
  // --component-rules (doc 15 §3.5): "off" (default), "report" or "soft" (detect; write the sidecar .kicad_dru next
  // to the output), "on" (also route with the generated keep-outs as in-memory rule areas; never written into the
  // output board).
  std::string component_rules = "off";
  // --reroute: remove every unlocked track and via first and route the whole board again (locked copper, arcs and
  // zones stay; rule 6).
  bool reroute = false;
  std::string warm_pcb;           // --warm-pcb: a routed board whose tracks and vias are the starting point (kept where legal under the input's rules; nets of one connection)
  std::string rules_override;     // --rules-override: user override file (JSON, doc 15 §6.3); empty = none
  // With soft zones: refill the zones after routing and route what the refill left unconnected (doc 05 §36).
  bool refill_repair = true;
  // --no-tracks-on / --layer-cost (doc 05 §29): copper layer names, and "LAYER=FACTOR" entries. Resolved against
  // the board into opt.no_track_layers / opt.layer_cost_pm; an unknown layer or a bad factor throws.
  std::vector<std::string> no_tracks_on;
  std::vector<std::string> layer_costs;
  // Buses (doc 05 §38): "NAME=PATTERN" groups (--bus), automatic detection (--buses) and the skew limit of the
  // command-line groups in mm (--bus-skew-mm; 0 = none). A custom skew rule of the board wins over the option.
  std::vector<std::string> buses;
  bool detect_buses = false;
  double bus_skew_mm = 0;
  // After-route passes (doc 05 §39), all off by default. Each changes the new copper only through the router's
  // exact check and is kept only if no net has more unconnected items with it than without (rule 4).
  // --widen-power: new tracks narrower than their net class width go back up to it where there is room; nets with
  // a target (--widen-net "PATTERN=WIDTH_MM", or a width the component rules computed) are widened towards it.
  bool widen_power = false;
  std::vector<std::string> widen_nets;
  // --stitch-vias: "planes" (vias of a plane's net on a grid where its fills overlap on two or more layers),
  // "return" (a plane via near each via of the signals that need a reference plane: component rules of kind
  // reference_plane, and --stitch-return "PATTERN=PLANE_NET"), or "all". Empty = off.
  std::string stitch_vias;
  double stitch_pitch_mm = 0;      // 0 = the catalogue's stitch_via_within_mm
  double stitch_distance_mm = 0;   // 0 = the rule's stitch_via_mm, else the catalogue value
  std::vector<std::string> stitch_returns;
  long pass_work = 2'000'000;      // work budget of each pass, in exact checks (rule 5)
  // Progress lines (the CLI's stdout text, one line per call, no trailing newline). Empty = silent.
  std::function<void(const std::string&)> log;
};

struct RouteJobResult {
  route::RouteResult result;
  nlohmann::json items;           // new copper in the --emit-items layout (layer and net by name, nm)
  nlohmann::json summary;         // the --json layout
  std::string viewer_url;         // empty when not viewing
  int unconnected_after_refill = -1;  // with soft zones: what KiCad's refill would leave unconnected
  // 0 when everything planned was routed (and, with soft zones, nothing is unconnected after the refill).
  int exit_code() const { return unconnected_after_refill >= 0 ? (unconnected_after_refill == 0 ? 0 : 3) : result.routed == result.connections ? 0 : 3; }
};

// Reads the board and its rules, routes (a portfolio when more than one variant runs), writes the requested outputs.
// Throws std::exception on unreadable input.
RouteJobResult run_route_job(RouteJob job);

// Buses (doc 05 §38): the groups `specs` ("NAME=PATTERN") name on the board, then, with `detect`, the groups found in
// the net names that share no net with an earlier group. Each group gets the skew limit of the board's custom
// rules, else, for groups from `specs`, `skew_mm` (0 = none). `notes` receives warnings. Throws on a bad spec.
std::vector<route::BusGroup> resolve_buses(const model::Board& b, const model::DesignRules& rules, const std::vector<std::string>& specs,
                                           bool detect, double skew_mm, std::vector<std::string>* notes = nullptr);
// A group measured on the copper of `b` (`joined`: route::nets_joined of the same board), as the JSON object of the
// route summary's `buses` array and of `tracemaker buses --json`; `line`, if given, gets the one-line text form.
// Lengths are KiCad's: tracks plus the stackup height of the vias (`rules` says whether the project counts it).
nlohmann::json bus_report(const model::Board& b, const model::DesignRules& rules, const std::vector<std::uint8_t>& joined, const route::BusGroup& g,
                          std::string* line = nullptr);

// The GPU device used for cost-to-go fields: the first listed device, or -1 (none, or `use_gpu` false).
int default_gpu_device(bool use_gpu);

}  // namespace tmk::app
