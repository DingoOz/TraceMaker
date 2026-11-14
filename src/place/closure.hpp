// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Placement closure (design doc 04 §11, D190): move components until the router completes the board.
//
// The input placement is routed; while connections stay unrouted, a multi-armed bandit chooses which kind of
// placement change to try next, from small ECO moves next to the failed connections to re-placing the whole board.
// One trial = one change of the incumbent placement, routed with the caller's deterministic work budget. A trial is
// kept only when it leaves fewer connections unrouted (transactions, CLAUDE.md rule 4). The bandit learns, on this
// board, which kinds pay off; a prior learned on other boards can be passed in, and this run's statistics are
// returned so the caller can store them.
//
// Arms (the kinds of change), cheapest first:
//   shift    one part next to a failure, 1–2 pitches in x or y          (doc 04 §4)
//   rotate   one part turned 90/180/270° about its body centre          (doc 04 §4)
//   swap     two interchangeable parts exchange places                  (doc 04 §4)
//   flip     one part to the other side (only with --flip, D48)         (doc 04 §4, §9)
//   room     every movable neighbour of a failed pad pushed away from it at once (pads the router reports as
//            "boxed in" first): makes escape room no single-part move makes
//   far      one part 3–8 pitches away
//   window   refine annealing restricted to the parts near the failures, with the failed nets boosted and the
//            routing capacity around them shrunk (doc 04 §3 G, one round)
//   replace  a new placement of the whole board (the caller's full-mode placement with the routability term; a
//            different seed on each pull), from which the small arms carry on
//
// Bandit: UCB1 (Auer, Cesa-Bianchi and Fischer, "Finite-time analysis of the multiarmed bandit problem", Machine
// Learning 47, 2002) on the success rate of each arm divided by its cost in routes, with a prior (pseudo-counts) so
// a run of 20–40 routes does not spend most of them exploring — the weakness of ALNS's segment-wise roulette
// weights (Ropke and Pisinger, Transportation Science 40(4), 2006) on short runs. After `stall` trials without
// improvement the next trial is radical (window or replace): the change escalates from slight to radical. The
// bandit's arithmetic is fixed-point, ties go to the lower arm index, and nothing depends on wall time except the
// job stop, so a run is reproducible.
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "place/routable.hpp"

namespace tmk::place {

enum Arm : int { kShift, kRotate, kSwap, kFlip, kRoom, kFar, kWindow, kReplace, kArms };
const char* arm_name(int arm);
int arm_from_name(const std::string& name);  // -1 if unknown

// Per-arm statistics: trials and improving trials. Also the learned prior stored between runs.
struct ArmStats {
  int pulls = 0, wins = 0;
};
using ArmTable = std::array<ArmStats, kArms>;

struct ClosureOptions {
  int max_routes = 40;          // trials routed (the input's route is the caller's)
  int stall = 6;                // trials without improvement before a radical trial is forced
  int window_per_incumbent = 4; // window trials (different seeds) per incumbent
  Coord pitch = 500'000;
  Coord corridor = 1'500'000;   // parts whose body is within this of a failed connection's box are candidates
  Coord room = 2'000'000;       // room arm: neighbours whose body is within this of a failed pad are pushed
  double alpha_cross_mm = 2.0, beta = 1.0, disp_cost = 0.25, room_gain = 4.0, via_mm = 0;
  int weight_boost = 3;
  double penalty = 0.6;         // window arm: capacity factor around the failures
  double explore = 0.5;         // UCB exploration weight
  int prior_cap = 4;            // a learned prior counts for at most this many pseudo-trials per arm
  const ArmTable* prior = nullptr;  // learned on other boards (null: the built-in prior)
  PlaceOptions place;           // window arm's annealing options (mode is forced to refine)
  // replace arm: the k-th whole-board placement (k = 0, 1, ...), legal on the loop's problem; nullopt = no more,
  // an empty placement = this one failed (counted as a trial without a route).
  std::function<std::optional<Placement>(int k)> radical;
  std::function<bool()> out_of_time;  // job wall-time stop, checked between trials
  std::function<void(const std::string&)> log;
  std::function<void(const std::string& label, const Placement& pl)> on_accept;  // recording only
};

struct ClosureTrial {
  int arm = 0;
  std::string what;
  int before = 0, after = 0;  // unrouted before (the incumbent) and with this change
  bool accepted = false;
  bool forced = false;        // a radical trial forced by the stall rule
};

struct ClosureResult {
  Placement pl;
  RouteEval eval;
  int routes = 0, accepted = 0;
  std::vector<ClosureTrial> trials;
  ArmTable stats{};           // this run's trials per arm
  std::string stop;           // "complete", "route budget", "time limit", "no candidates"
};

// Expected success rate of each arm before any trial on this board, in parts per million, and its cost in routes
// (window and replace also anneal). From the M8 results (doc 04 §8.3): small shifts and rotations closed most boards.
std::array<int, kArms> default_prior_ppm();
std::array<int, kArms> arm_cost();

ClosureResult placement_closure(const Problem& p, const Placement& start, const RouteEval& start_eval, const ClosureOptions& o,
                                const RouteFn& route);

}  // namespace tmk::place
