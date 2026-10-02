#pragma once
// TraceMaker's detailed router, version 1 (roadmap M4; design doc 05 §2, §5).
//
// Octilinear A* on a fine lattice (optimal under its cost model), with legality decided lazily by exact
// clearance tests against the DRC's rule engine, then exact verification of every committed segment and via.
#include <cstdint>
#include <string>
#include <vector>

#include "core/events.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

struct RouterOptions {
  Coord pitch = 0;              // lattice pitch; 0 = automatic from net-class widths and clearances
  double time_limit_s = 120;    // wall-clock budget for the whole run
  long max_expansions = 4'000'000;  // per search attempt
  int max_attempts = 4;         // per connection (window growth and learned blocks between attempts)
  double via_cost_mm = 1.0;     // equivalent track length of one via
  bool allow_vias = true;
  std::uint64_t seed = 1;
  events::Sink* sink = nullptr;
};

struct Connection {
  model::NetId net = 0;
  int pad_a = -1, pad_b = -1;   // board pad indices
  Coord length = 0;             // straight-line distance
};

struct RouteResult {
  std::vector<model::Track> tracks;  // new copper, in commit order
  std::vector<model::Via> vias;
  int connections = 0, routed = 0;
  long expansions = 0;
  double seconds = 0;
  Coord pitch = 0;
  std::vector<std::string> failures;  // one line per unrouted connection
};

class Router {
 public:
  Router(const model::Board& board, const model::DesignRules& rules, RouterOptions opt);
  RouteResult run();

 private:
  struct Impl;
  const model::Board& in_;
  const model::DesignRules& rules_;
  RouterOptions opt_;
};

}  // namespace tmk::route
