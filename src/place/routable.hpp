#pragma once
// Router-in-the-loop placement (design doc 04 §3 G) and ECO placement (doc 04 §4).
//
// Both take the router as a callback (`RouteFn`), so this library does not depend on tm::route and the logic
// can be tested with a synthetic router. The caller routes with a deterministic work budget, so every decision
// here is reproducible. A placement is only ever accepted when it routes more connections than the incumbent
// (transactions, CLAUDE.md rule 4); locked and otherwise fixed parts never move (rule 6).
#include <functional>
#include <string>
#include <vector>

#include "place/congestion.hpp"
#include "place/placer.hpp"

namespace tmk::place {

// What the router reports for one placement.
struct RouteEval {
  bool ok = false;
  int connections = 0, routed = 0;
  int unrouted() const { return connections - routed; }
  struct Failure {
    int part_a = -1, part_b = -1;  // parts at both ends (-1: a zone or an unknown reference)
    Point a, b;                    // end points (pad positions; b = a for zone connections)
    std::string net;
  };
  std::vector<Failure> failed;
  double seconds = 0;
};
using RouteFn = std::function<RouteEval(const Placement&)>;

struct Candidate {
  std::string label;
  Placement pl;
  RouteEval eval;
  std::int64_t hpwl = 0;  // Σ HPWL (nm) on the loop's problem
};
// Better routed result: fewer unrouted connections, then shorter wirelength.
bool better(const Candidate& a, const Candidate& b);

// ---- ECO placement (doc 04 §4) ---------------------------------------------------------------------------------

struct EcoOptions {
  int rounds = 4;              // commit rounds
  int candidates = 5;          // moves routed per round (the best-ranked ones)
  Coord pitch = 500'000;       // shift step
  int max_steps = 4;           // shifts of 1..max_steps pitches in each axis direction
  Coord corridor = 1'500'000;  // parts whose body is within this of a failed connection's box are candidates
  double alpha_cross_mm = 2.0;
  double beta = 1.0;           // routability weight in the ranking cost
  int weight_boost = 3;        // failed nets weigh this many times more in the ranking cost
  double disp_cost = 0.25;     // ranking: one mm of displacement costs this many mm of signal HPWL
  std::function<void(const std::string&)> log;
};

struct EcoResult {
  Placement pl;
  RouteEval eval;
  int routes = 0, committed = 0, ranked = 0;
  std::vector<std::string> moves;  // committed moves ("R3 shift +x 1.0 mm: 5 -> 3 unrouted")
};

// Small moves of movable parts near the failed connections of `start` (shift, rotate, swap with an identical
// part), ranked by a geometric estimate (annealing cost with boosted failed nets, penalised congestion bins and a
// displacement charge), the top ones routed; the best strictly improving move is committed per round. Moves that
// failed are remembered and not tried again.
EcoResult eco_place(const Problem& p, const Placement& start, const RouteEval& start_eval, const EcoOptions& o, const RouteFn& route);

// ---- Routability loop (doc 04 §3 G) ----------------------------------------------------------------------------

struct LoopOptions {
  PlaceOptions place;           // annealing options for the re-placement rounds (mode is forced to refine)
  double beta = 1.0;            // routability weight (RUDY overflow) in the re-placement rounds
  int rounds = 3;               // penalty rounds after the seeds
  double penalty = 0.6;         // capacity factor applied per round to bins around failed connections
  int weight_boost = 3;         // failed nets weigh this many times more in the re-placement rounds
  int eco_candidates = 4;       // ECO moves routed per round (0 = no ECO step)
  std::function<void(const std::string&)> log;
};

struct LoopResult {
  Candidate best;
  std::vector<Candidate> tried;  // every routed candidate (placements dropped), in order
  int routes = 0, rounds = 0;
};

// Routes the seeds (those without a result), keeps the best, then for up to `rounds` rounds re-places around the
// failed connections (refine annealing with β·RUDY over a capacity map shrunk where the router failed, boosted
// weights for the failed nets, LNS windows centred on their parts) and tries ECO moves; a candidate replaces
// the incumbent only if it leaves fewer connections unrouted.
LoopResult routability_loop(const Problem& p, std::vector<Candidate> seeds, const LoopOptions& o, const RouteFn& route);

}  // namespace tmk::place
