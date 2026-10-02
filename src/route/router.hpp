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
  double time_limit_s = 120;    // wall-clock safety limit for the whole run
  long work_budget = 0;         // deterministic budget in search expansions (0 = none): same input + seed => same output
  long max_expansions = 3'000'000;  // per search attempt
  bool bend_states = true;          // direction in the A* state (exact bend costs) vs. parent-direction approximation
  double heuristic_weight = 1.0;
  bool field_heuristic = true;      // GPU cost-to-go field as the A* heuristic on large windows
  int field_min_cells = 60'000;     // window size (lattice points x layers) from which the field is used
  int gpu_device = 0;               // CUDA device for fields (-1 = none)    // weighted A* (1.0 = optimal under the cost model; >1 trades optimality for speed)
  int max_attempts = 4;         // per connection (window growth and learned blocks between attempts)
  double via_cost_mm = 1.0;     // equivalent track length of one via
  bool allow_vias = true;
  bool rip_up = true;           // negotiated rip-up and reroute (design doc 05 §6 rung R2, doc 06 §3)
  int max_rips_per_connection = 8;
  int max_passes = 12;          // passes over still-unrouted connections
  int max_restarts = 6;         // full restarts (hardest first, history kept) when negotiation stalls
  double soft_cost_mm = 1.0;    // base cost of crossing another net's routed copper (before history)
  std::uint64_t seed = 1;
  int order = 0;                // connection order: 0 shortest first, 1 longest first, 2 shortest first with seeded jitter
  // Connections to route first ("REF.NUM" pairs, either orientation): learned from earlier failures (doc 06 T3).
  std::vector<std::pair<std::string, std::string>> priority;
  std::string only_net;         // debugging: route only this net
  events::Sink* sink = nullptr;
};

struct Connection {
  model::NetId net = 0;
  int pad_a = -1, pad_b = -1;   // board pad indices (pad_b = -1 when the target is a zone fill)
  int zone_b = -1;              // copper item index of a zone fill (plane) to connect into, or -1
  Coord length = 0;             // straight-line distance
};

struct RouteResult {
  std::vector<model::Track> tracks;  // new copper, in commit order
  std::vector<model::Via> vias;
  int connections = 0, routed = 0;
  long expansions = 0;
  int rips = 0, passes = 0;
  int enclosed = 0;             // searches that proved the source boxed in (no larger window tried)
  long nogood_skips = 0;        // attempts skipped because an identical attempt already failed
  int necked = 0;               // connections routed at the neck-down width
  int restarts = 0;
  double seconds = 0;
  Coord pitch = 0;
  std::vector<std::string> failures;  // one line per unrouted connection
  struct Unrouted {
    std::string net, a, b;  // "REF.NUM" (b = "zone" for plane connections)
  };
  std::vector<Unrouted> unrouted;
};

// Runs several differently configured routers in parallel threads and keeps the best result (most connections
// routed, then fewest vias, then shortest copper). The first variant is `base` itself; only it streams events.
struct PortfolioResult {
  RouteResult best;
  int best_variant = 0;               // position in `variants`
  std::vector<int> indices;            // portfolio variant index per position
  std::vector<std::string> variants;   // description per variant
  std::vector<int> routed;             // routed count per variant
};
// `pick`: which variant indices to run (empty = the first `threads`).
PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base, int threads,
                                const std::vector<int>& pick = {});
int portfolio_size();

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
