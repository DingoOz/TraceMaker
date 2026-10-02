#pragma once
// (E) Detailed placement by simulated annealing (design doc 04 §3 E; Kirkpatrick, Gelatt, Vecchi, Science
// 1983; move set after TimberWolf, Sechen & Sangiovanni-Vincentelli, IEEE JSSC 1985).
//
// Moves: shift (radius shrinks with temperature), pull towards the weighted median of the connected pins,
// rotate by 90/180/270°, swap two interchangeable parts (same footprint, side and orientation class). Every
// move is checked exactly before it is applied, so the state is always legal. Cost (integer, incremental):
// weighted HPWL + α · (crossings between minimum-spanning-tree airwires of different signal nets), the
// crossings counted against a uniform segment grid. Independent runs with different seeds go in parallel and
// the best is kept (ties: lowest run index); run budgets are counted in moves, so results do not depend on
// thread timing unless the wall-time limit stops a run.
#include <cstdint>
#include <vector>

#include "place/legality.hpp"

namespace tmk::place {

struct AnnealOptions {
  std::uint64_t seed = 1;
  int runs = 8;
  int threads = 8;
  double effort = 1.0;          // moves per run = effort × 4000 × movable parts (at least 20 000)
  double alpha_cross_mm = 2.0;  // one airwire crossing costs as much as this much signal HPWL
  double time_limit_s = 0;      // wall-time stop (0 = none); hitting it makes the result timing-dependent
  bool refine = false;          // start cool and with small moves (the start is already a good placement)
};

struct AnnealResult {
  Placement pl;
  std::int64_t cost = 0, whpwl = 0, crossings = 0;
  std::int64_t start_cost = 0;
  int best_run = -1;
  std::uint64_t moves = 0, accepted = 0, illegal = 0;
  bool time_limited = false;
  std::vector<std::int64_t> run_costs;
};

// `start` must be legal for every movable part.
AnnealResult anneal(const Problem& p, const Placement& start, const AnnealOptions& o);

// Cost used by the annealer (for tests): weighted HPWL + α·crossings with α in the same integer units.
std::int64_t anneal_cost(const Problem& p, const Placement& pl, double alpha_cross_mm);

}  // namespace tmk::place
