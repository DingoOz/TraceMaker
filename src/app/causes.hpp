// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Why connections stay open on a routed board (M20 C5, doc 05 §55): each open connection is classified by what has to change for the router to
// route it. The router is run again on copies of the board, its copper locked, with other nets' copper taken away: near the connection's ends
// only the nets that pass through (no pad on an end's part), then all of it near the ends, then all of it away from them, then everywhere. The
// first copy on which the connection routes names the cause; one on which it never routes is "no_path_bare". A pin that cannot leave its
// package under the rules on the bare board (route/escape.hpp) is "pin_cannot_escape", without a run.
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace tmk::app {

struct CausesOptions {
  std::string rules_from;           // read the rules beside this board instead (a routed copy may have lost its project file)
  long baseline_work = 10'000'000;  // work of the run on the board as it is (connections it routes were left by the budget or the order)
  long work = 2'000'000;            // work of each run that routes one net on a changed copy
  double radius_mm = 2.0;           // "near an end": within this distance of an end pad's box
  int threads = 8;                  // nets classified at once (the result does not depend on it)
  std::uint64_t seed = 1;
  bool baseline_only = false;       // only the run on the board as it is (the layer test: run it on a copy with more layers)
};

// Classifies the open connections of `board` and returns the report (also printed as a table when `print`).
nlohmann::json classify_open_connections(const std::string& board, const CausesOptions& opt, bool print);

}  // namespace tmk::app
