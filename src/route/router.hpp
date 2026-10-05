#pragma once
// TraceMaker's detailed router, version 1 (roadmap M4; design doc 05 §2, §5).
//
// Octilinear A* on a fine lattice (optimal under its cost model), with legality decided lazily by exact
// clearance tests against the DRC's rule engine, then exact verification of every committed segment and via.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/events.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

struct RouterOptions {
  Coord pitch = 0;              // lattice pitch; 0 = automatic from net-class widths and clearances
  double pitch_scale = 1.0;      // auto pitch multiplier, applied only on large lattices (>= 3M points per layer)
  double time_limit_s = 120;    // wall-clock safety limit for the whole run
  long work_budget = 0;         // deterministic budget in search expansions (0 = none): same input + seed => same output
  // Per search attempt. One variant at a fixed budget prefers 1M (10 hard boards: 3M / 2M / 1M / 750k / 500k routed
  // 8,285 / 8,309 / 8,381 / 8,371 / 8,363), but the 8-variant 120 s tiers did not confirm it (tier B 5,167 -> 5,161,
  // tier C 9,523 -> 9,512 and 63.3 % -> 60.0 % clean), so it stays 3M (doc 05 §13; --max-expansions to experiment).
  long max_expansions = 3'000'000;
  bool reach_verify = false;        // test only: run the A* even where the pre-check proved no path, count mismatches
  int reach_check = 1;              // flood-fill reachability check before strict searches (exact): 0 off, 1 likely failures, 2 all
  bool bend_states = true;          // direction in the A* state (exact bend costs) vs. parent-direction approximation
  double heuristic_weight = 1.0;
  bool field_heuristic = true;      // GPU cost-to-go field as the A* heuristic on large windows
  int field_min_cells = 60'000;     // window size (lattice points x layers) from which the field is used
  int gpu_device = 0;               // CUDA device for fields (-1 = none)    // weighted A* (1.0 = optimal under the cost model; >1 trades optimality for speed)
  int max_attempts = 4;         // per connection (window growth and learned blocks between attempts)
  int soft_attempts = 3;        // window sizes tried by negotiated searches
  double via_cost_mm = 3.0;     // equivalent track length of one via
  bool allow_vias = true;
  bool rip_up = true;           // negotiated rip-up and reroute (design doc 05 §6 rung R2, doc 06 §3)
  int max_rips_per_connection = 8;
  int max_passes = 12;          // passes over still-unrouted connections
  bool blind_vias = false;      // use blind/buried vias where a through via is blocked, if the board allows them
  bool diff_pairs = false;      // route P/N pairs together as coupled tracks first (falls back to single routing)
  // Component rules (doc 15 P3): these net pairs only are routed coupled first when diff_pairs is off (e.g. USB 2.0
  // D+/D- bound by tm::crules, whose names need not end in P/N or +/-). Empty = none.
  std::vector<std::pair<model::NetId, model::NetId>> pair_nets;
  bool global_route = false;    // plan every connection on a coarse tile graph first; detailed search follows the corridors
  // Global router v2 (M6): the first search of each connection is confined to its corridor (cells outside are
  // blocked, window cropped to the corridor); only if that fails do the usual unconfined windows run.
  bool global_confine = false;
  // Strict pass with corridors: a connection that cannot be routed inside its corridor goes straight to
  // negotiation instead of flooding the wide strict windows (on logicbone those failed floods were 76 % of all
  // search expansions; doc 05 §13).
  bool global_strict_corridor_only = false;
  bool optimize = true;         // post-routing clean-up: re-route connections to save vias and length
  bool escape_plan = false;     // reserve escape corridors for the pins of dense packages (route/escape.hpp, M9)
  bool escape_second_ring = false;  // second-ring balls escape between two outer balls instead of by dog-bone
  int max_restarts = 6;         // full restarts (hardest first, history kept) when negotiation stalls
  double soft_cost_mm = 1.0;    // base cost of crossing another net's routed copper (before history)
  std::uint64_t seed = 1;
  int order = 0;                // connection order: 0 shortest first, 1 longest first, 2 shortest first with seeded jitter
  // Connections to route first ("REF.NUM" pairs, either orientation): learned from earlier failures (doc 06 T3).
  std::vector<std::pair<std::string, std::string>> priority;
  std::string only_net;         // debugging: route only this net
  events::Sink* sink = nullptr;
  // Shared by portfolio variants (wall-clock mode only): when one variant has routed everything at time T, the
  // others may continue until 2T + 5 s; a variant that is complete always finishes its clean-up.
  std::atomic<double>* deadline = nullptr;
  // Recording: every variant buffers its events and only the winning variant's are forwarded to `sink`
  // (each message gains a leading "t" field, seconds since that variant started).
  bool buffer_events = false;
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
  int optimized = 0;
  int pairs = 0;
  int length_tuned = 0;
  int blind_vias = 0;           // blind/buried vias placed
  int escape_corridors = 0;     // escape corridors reserved (M9)          // nets brought into their custom length range by meanders                // differential pairs routed coupled            // connections improved by the clean-up pass
  double seconds = 0;
  Coord pitch = 0;
  std::vector<std::string> failures;  // one line per unrouted connection
  struct Unrouted {
    std::string net, a, b;  // "REF.NUM" (b = "zone" for plane connections)
  };
  std::vector<Unrouted> unrouted;
};

// Runs several differently configured routers (portfolio variants) and keeps the best result. Variant i is a pure
// function of (base options, i): it never depends on the thread count, and with a work budget each variant's result
// is deterministic. The variants are scheduled on `threads` worker threads (0 = one per variant) in index order and
// the winner is the minimum of a total order (most connections routed, then fewest vias, then shortest copper, then
// the lowest variant index), so with `base.work_budget` > 0 the output is the same for any `threads` (requirement
// N3, decision D47). Only the first variant streams events live.
struct PortfolioResult {
  RouteResult best;
  int best_variant = 0;               // position in `variants`
  std::vector<int> indices;            // portfolio variant index per position
  std::vector<std::string> variants;   // description per variant
  std::vector<int> routed;             // routed count per variant
};
// `variants`: how many variants to run (the first `variants` of the portfolio); `pick`: which variant indices to
// run instead (non-empty: overrides `variants`). `threads`: concurrency only (0 = one thread per variant).
PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base, int variants,
                                const std::vector<int>& pick = {}, int threads = 0);
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
