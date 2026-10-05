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
//
// M8 additions (doc 04 §3 E, G):
// - Parallel tempering (replica exchange; Swendsen & Wang, PRL 57, 1986; Hukushima & Nemoto, J. Phys. Soc. Jpn.
//   65, 1996): `runs` replicas at geometric temperatures run fixed-length sweeps in parallel; between sweeps,
//   neighbouring temperatures exchange replicas with the Metropolis criterion, decided sequentially from a
//   dedicated random stream, so the result does not depend on the thread count.
// - Large-neighbourhood search (Shaw, "Using constraint programming and local search methods to solve vehicle
//   routing problems", CP 1998): rip up a window of nearby parts and re-place them, exactly (branch and bound
//   over candidate slots and rotations) for small windows, greedily otherwise; kept only if the cost falls.
// - A routability term β · RUDY overflow (place/congestion.hpp).
//
// Side assignment (doc 04 §3 C/E, D48; opt-in): a flip move puts a part on the other side (KiCad's mirrored
// footprint, problem.hpp kStates) at its body centre or at the weighted median of its nets, and the cost gains
// a via estimate for nets with surface-mount pins on both sides.
#include <chrono>
#include <cstdint>
#include <vector>

#include "place/congestion.hpp"
#include "place/legality.hpp"

namespace tmk::place {

// Recording clock (video timelapse): steady-clock seconds. Only ever written to recordings.
inline double trace_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct AnnealOptions {
  std::uint64_t seed = 1;
  int runs = 8;
  int threads = 8;
  double effort = 1.0;          // moves per run = effort × 4000 × movable parts (at least 20 000)
  double alpha_cross_mm = 2.0;  // one airwire crossing costs as much as this much signal HPWL
  double time_limit_s = 0;      // wall-time stop (0 = none); hitting it makes the result timing-dependent
  bool refine = false;          // start cool and with small moves (the start is already a good placement)
  // Routability: cost of 1 mm of RUDY overflow in mm of signal HPWL (0 = off; resolution 0.1). `congestion`
  // is the capacity map (built with default options when null).
  double beta_congestion = 0;
  const CongestionMap* congestion = nullptr;
  bool tempering = false;       // parallel tempering over `runs` replicas instead of independent runs
  int exchange_interval = 0;    // moves per replica between exchange attempts (0 = auto)
  double lns_rate = 0;          // probability that a step is an LNS window instead of an ordinary move
  int lns_window = 6;           // parts per LNS window (≤ 12)
  int exact_window = 4;         // windows up to this size are repaired exactly (branch and bound)
  std::vector<int> focus;       // parts LNS windows are centred on (empty: any movable part)
  std::uint64_t trace_every = 0;  // record the live state every this many steps (0 = off; video recording)
  // Side assignment (doc 04 §3 C/E, D48): flip moves for parts with Part::may_flip(), and the via estimate
  // via_mm · Σ w · min(front, back surface-mount pins) per net in the cost (wirelength.hpp side_vias).
  bool flip = false;
  double via_mm = 2.0;          // one estimated via costs as much as this much signal HPWL
  double flip_rate = 0.1;       // share of ordinary moves that are flips (only drawn when flipping is on)
};

struct AnnealResult {
  Placement pl;
  std::int64_t cost = 0, whpwl = 0, crossings = 0;
  std::int64_t start_cost = 0;
  int best_run = -1;
  std::uint64_t moves = 0, accepted = 0, illegal = 0;
  bool time_limited = false;
  std::vector<std::int64_t> run_costs;
  std::int64_t overflow = 0;    // RUDY overflow of the result (nm; 0 when β = 0)
  std::uint64_t exchanges_tried = 0, exchanges_accepted = 0;
  std::uint64_t lns_tried = 0, lns_improved = 0;
  std::vector<Placement> trace;  // the winning run's recorded states (trace_every > 0)
  std::vector<double> trace_t;   // when each was recorded (trace_now())
};

// `start` must be legal for every movable part.
AnnealResult anneal(const Problem& p, const Placement& start, const AnnealOptions& o);

// Cost used by the annealer, from scratch (reference path): weighted HPWL + α·crossings + β·RUDY overflow,
// all in the annealer's integer units.
std::int64_t anneal_cost(const Problem& p, const Placement& pl, double alpha_cross_mm, double beta = 0,
                         const CongestionMap* m = nullptr, double via_mm = 0);

// Large-neighbourhood search from `start` (legal): `windows` LNS windows (sizes and seeds from `o`; the
// stream is o.seed). Never returns a placement with a higher cost than `start`.
struct LnsResult {
  Placement pl;
  std::int64_t cost_before = 0, cost_after = 0;
  std::uint64_t tried = 0, improved = 0;
};
LnsResult lns_improve(const Problem& p, const Placement& start, const AnnealOptions& o, int windows);

// Exact window (doc 04 §3 F, without CP-SAT): the best placement of `parts` (others fixed) over a candidate
// set per part (its own spot, the other window parts' spots, its HPWL-optimal spot; four rotations each),
// by branch and bound with an HPWL bound. `prune` = false enumerates every combination (reference path).
// `pl` is updated only when the optimum is strictly better than the start.
struct WindowResult {
  bool improved = false, proven = false;  // proven: the search finished within max_leaves
  long nodes = 0, leaves = 0;
  std::int64_t cost_before = 0, cost_after = 0;
};
WindowResult solve_window(const Problem& p, Placement& pl, const std::vector<int>& parts, const AnnealOptions& o, bool prune = true,
                          long max_leaves = 200'000);

}  // namespace tmk::place
