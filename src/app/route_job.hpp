// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One `tracemaker route` run as a reusable function, shared by the CLI (src/app/main.cpp) and the Python bindings
// (bindings/), so both take exactly the same code path (design doc 08 §5).
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

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
  std::string rules_override;     // --rules-override: user override file (JSON, doc 15 §6.3); empty = none
  // Progress lines (the CLI's stdout text, one line per call, no trailing newline). Empty = silent.
  std::function<void(const std::string&)> log;
};

struct RouteJobResult {
  route::RouteResult result;
  nlohmann::json items;           // new copper in the --emit-items layout (layer and net by name, nm)
  nlohmann::json summary;         // the --json layout
  std::string viewer_url;         // empty when not viewing
  int exit_code() const { return result.routed == result.connections ? 0 : 3; }
};

// Reads the board and its rules, routes (a portfolio when more than one variant runs), writes the requested outputs.
// Throws std::exception on unreadable input.
RouteJobResult run_route_job(RouteJob job);

// The GPU device used for cost-to-go fields: the first listed device, or -1 (none, or `use_gpu` false).
int default_gpu_device(bool use_gpu);

}  // namespace tmk::app
