// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The stuck report (design doc 17 §3, issue #26 stage 0): what failed and why, as JSON that an advisor (a person, a script
// or a model) can act on. It is built from a finished route, is useful on its own (`route --stuck-report FILE`), and is
// what `route --advisor CMD` sends to the advisor on stdin.
//
// The report quotes names from the board (nets, parts), which makes it a prompt-injection surface and, sent to a cloud
// model, a disclosure. So it says exactly what it contains (`disclosure`), is limited in size, and can be redacted: with
// `redact` every net and part gets an alias (N1, P3) and the advice that comes back uses the aliases (advice::unalias).
#include <string>

#include <nlohmann/json.hpp>

#include "advice/advice.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/router.hpp"

namespace tmk::advice {

struct StuckInput {
  const model::Board* board = nullptr;
  const model::DesignRules* rules = nullptr;
  const route::RouteResult* result = nullptr;
  std::string board_name;                 // the file name, no path
  double seconds = 0;                     // wall time of the run that produced the result
  nlohmann::json variants;                // rows {"variant", "name", "routed", "seconds"} of that run; null = none
  nlohmann::json previous_advice;         // the report of advice already tried (Applied::report) with its outcome; null = none
  bool redact = false;                    // aliases for net and part names; no part values or library ids
  int max_unrouted = 40;                  // the longest unrouted list sent (the rest are counted)
  double region_margin_mm = 1.5;          // around the two pads of a connection: the region whose crowding is described
  // Escape analysis of the dense packages (route/escape.hpp, the `escape` command): which pins cannot leave their package at all under
  // the board's rules, and the analysis's own hint. Cheap (0.2 s on a 163-ball array). Run with the same obstacle options as the route.
  bool escape_analysis = true;
  bool soft_zones = false;
  double vias_off_pads_below_mm = 0;
};

struct StuckReport {
  nlohmann::json json;
  AliasMap aliases;                       // filled when redacting
};

StuckReport build_stuck_report(const StuckInput& in);

}  // namespace tmk::advice
