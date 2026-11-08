// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Repair windows (issue #56, doc 05 §73, D172): finish a nearly routed board without routing all of it again.
//
// A plain run keeps every track and via of the input as fixed copper, so a pin that the neighbours' fan-outs wall in
// stays open ("fixed copper encloses the pin"). The repair lifts the unlocked copper near each open connection (whole
// pieces: the tracks and vias of one net between its pads), routes the board again with what is left fixed and the
// open connection first, and keeps the result only if no pads that were joined come apart and fewer connections are
// open (rule 4). On failure the window grows. Ripping is limited to the window: everything outside stays as it was.
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/router.hpp"

namespace tmk::passes {

struct RepairOptions {
  std::vector<double> radii_mm{1, 2, 4};  // window radii around each end of an open connection, tried in turn
  long step_work = 3'000'000;             // work budget of each routing attempt (search expansions, rule 5)
  int max_branches = 64;                  // a window that would lift more branches ends the growth for that connection
  int max_conns = 64;                     // open connections tried at most (the first ones of the router's list)
  double time_limit_s = 0;                // > 0: no attempt starts after this many seconds (safety only; work decides)
  std::vector<model::NetId> keep_nets;    // nets whose copper is never lifted (coupled pairs, bus members)
};

// One routing attempt: an open connection, a window radius, what it lifted, and whether it was kept.
struct RepairStep {
  std::string conn;  // "NET A-B"
  double radius_mm = 0;
  int pieces = 0, tracks = 0, vias = 0;  // copper lifted (pieces: branches)
  int open_before = 0, open_after = 0;   // open connections of the board (pads not joined), before and after the attempt
  bool split = false;                    // some pads that were joined came apart
  bool kept = false;
  long work = 0;
};

struct RepairResult {
  bool changed = false;
  // The board's own tracks and vias that were lifted (indices into the `base` board given), ascending.
  std::vector<int> ripped_tracks, ripped_vias;
  // The new copper after the repair: what is left of the run's new copper, then the repair's, in commit order.
  std::vector<model::Track> tracks;
  std::vector<model::Via> vias;
  int open_before = 0, open_after = 0;
  // Of the last kept attempt (pad indices of `base`): the router's open connections and its explanations.
  std::vector<route::RouteResult::Unrouted> unrouted;
  std::vector<std::string> failures;
  std::vector<RepairStep> steps;
  long work = 0;
  double seconds = 0;
  nlohmann::json summary() const;
};

// Open connections of `b`, counted as the router does: per net, the groups of its pads that copper joins, less one.
int open_connections(const model::Board& b, const model::DesignRules& rules);

// Repairs the board `base` with the new copper of `res` (a run of `opt` on `base`). `log` gets one line per attempt.
// Deterministic: same inputs, same result (the attempts run in order, each under a work budget).
RepairResult repair_open(const model::Board& base, const model::DesignRules& rules, const route::RouteResult& res, const route::RouterOptions& opt,
                         const RepairOptions& ro, const std::function<void(const std::string&)>& log = {});

}  // namespace tmk::passes
