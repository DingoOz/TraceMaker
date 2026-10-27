// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One `tracemaker route` run as a reusable function, shared by the CLI (src/app/main.cpp) and the Python bindings
// (bindings/), so both take exactly the same code path (design doc 08 §5).
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "analysis/footprint_check.hpp"
#include "analysis/return_path.hpp"
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
  // --report FILE (doc 18 D3, D136): a run report for designers, HTML (or Markdown for .md), rendered from the summary alone. It adds
  // the rules read and not read, the stuck report and a DRC of the board before and after to the summary, and writes the summary
  // to `json_out`, else beside the report as FILE.json, so every number in the report is a field of a file the reader has.
  std::string report_out;
  nlohmann::json options_given;   // the options as given on the command line and in --config, [{"name", "value"}]; reported only
  bool view = false;              // stream to the browser viewer
  // Reference-return audit of selected nets on the routed board (issue #7, doc 05 §42): off unless a selector is given. Read-only; the
  // report goes to the log and the summary, and each net's return path to the viewer.
  analysis::ReturnPathOptions return_paths;
  // Critical nets (issue #24, doc 05 §44, D117): route critical signals first, give them dearer vias, keep them over solid reference copper. Off
  // unless asked; `si_rules` turns the three on with the settings below unless they were given.
  std::vector<std::string> critical_nets;  // KiCad wildcards: nets that are critical whatever their role (tier 1)
  bool critical_auto = false;              // also the nets the component rules' roles make critical, and differential pairs
  bool critical_first = false;             // route them before all other nets, tier by tier
  double critical_via_mult = 1;            // a via costs them this many times as much
  bool critical_via_given = false;         // set by the command line: with si_rules, a value that was given is kept (else x3)
  int critical_via_max = 0;                // warn when a critical net has more vias than this (0 = no limit)
  double split_cost = 0;                   // pitches per lattice step over a void of the reference plane
  bool split_cost_given = false;           // as critical_via_given (else +4 with si_rules)
  bool split_hard = false;                 // such cells are closed to critical nets
  bool si_rules = false;
  // --footprint-refs (issue #5, doc 05 §43): every footprint is checked against the datasheet land patterns of a reference library before the first track is
  // routed; a discrepancy is an error and stops the job unless footprint_warn_only.
  std::string footprint_refs;     // the library: a directory or a JSON file; empty = no check
  std::string footprint_trusted;  // trusted brand list; empty = trusted_brands.txt beside the references
  bool footprint_trust_all = false;
  bool footprint_warn_only = false;
  analysis::FootprintCheckOptions footprint_opt;
  bool view_freeze = true;        // the viewer holds the scene on a routing stall (issue #8); --no-freeze says false in the board snapshot
  std::string view_host;          // "" = $TM_VIEW_HOST, else loopback: a viewer on any other address needs a token (issue #4, D116)
  std::string view_token;         // the token for a non-loopback viewer (empty = a random one)
  bool view_insecure = false;     // a non-loopback viewer without a token (--view-insecure)
  std::string view_embed;         // pages that may frame the viewer (--view-embed); empty = none
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
  // --copper-layers N (doc 05 §53): route the board with N copper layers, inner layers added (or empty ones removed) in memory and in the
  // output; 0 = as the file has them. `route --try-layers` sets it for each count it tries.
  int copper_layers = 0;
  // --pour (doc 05 §53, M20 C3): zones to add before routing, "auto" or "NET@LAYER" (passes::plan_pours). They are routed with soft
  // zones (switched on with them) and written into the output with the engine's fill.
  std::vector<std::string> pours;
  // Zones without a fill (doc 05 §54): filled in memory before routing and routed with soft zones, so a plane net's pads drop a via into
  // their plane instead of being joined by tracks; --no-fill-planes turns it off (the zones are then ignored while routing).
  bool fill_planes = true;
  std::string warm_pcb;           // --warm-pcb: a routed board whose tracks and vias are the starting point (kept where legal under the input's rules; nets of one connection)
  std::string rules_override;     // --rules-override: user override file (JSON, doc 15 §6.3); empty = none
  // Advice (doc 17, issue #26). The engine never talks to a model: it reads an advice file, and an advisor (any program
  // that turns a stuck report into an advice file) is consulted only when the deterministic route leaves connections open.
  std::string advice;             // --advice FILE: typed hints (src/advice/advice.hpp); advised variants run next to the unadvised ones
  int advice_variants = 0;        // --advice-variants N: advised variants (0 = the file's "variants", else 4)
  std::string advisor;            // --advisor CMD: a shell command that reads the stuck report on stdin and writes advice on stdout
  double advisor_timeout_s = 120; // --advisor-timeout S: the advisor's time limit (its wall time is not routing work)
  int advisor_rounds = 1;         // --advisor-rounds N: consult again with the result of the advised run while connections are left
  bool advisor_redact = false;    // --advisor-redact: the report names nets and parts by alias (N1, P3); the advice is mapped back
  std::string advice_out;         // --advice-out FILE: where the advice that was used is saved (default: <out>.advice.json)
  std::string stuck_report_out;   // --stuck-report FILE: write the stuck report (of the run the advisor saw, else the final one)
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

// The layer-count advisor (`route --try-layers`, doc 05 §53, M20 C3): routes the board with each copper layer count in `counts`,
// fewest first, and stops at the first that completes (exit code 0: everything routed and, with soft zones, nothing unconnected after
// the refill). Its board is written to `job.out` and its summary returned, with a "layers" object listing every count tried. A count
// the board cannot take (inner layers that are not empty) is reported and skipped. When none completes, the result of the count that
// left the fewest connections open is returned (the first such count on a tie).
RouteJobResult run_layer_advisor(RouteJob job, std::vector<int> counts);

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
