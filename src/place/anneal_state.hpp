#pragma once
// Internal: the incremental annealing state shared by simulated annealing, parallel tempering, large-
// neighbourhood search and exact windows (anneal.cpp, tempering.cpp, lns.cpp). Not part of the public API.
//
// The state is always legal for the movable parts (every committed move passed the exact test). Its cost is an
// exact integer, kept incrementally: Σ w·HPWL + α·crossings + β·RUDY overflow.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/rng.hpp"
#include "place/anneal.hpp"
#include "place/congestion.hpp"
#include "place/wirelength.hpp"

namespace tmk::place::detail {

inline constexpr Coord kLattice = 10'000;  // committed positions on a 10 µm lattice
inline constexpr int kMaxMove = 12;        // parts in one compound move (LNS windows)
inline constexpr double kLnsPhase = 0.3;   // LNS windows run in the last 30 % of an annealing budget

inline Coord snap(Coord v) { return (v >= 0 ? (v + kLattice / 2) : (v - kLattice / 2)) / kLattice * kLattice; }
inline Point snap(Point q) { return Point{snap(q.x), snap(q.y)}; }
inline std::int64_t alpha_units(double alpha_mm) { return static_cast<std::int64_t>(std::llround(alpha_mm * 1e6)) * kSignalWeight; }
// Cost of one nm of RUDY overflow: β (mm of signal HPWL per mm of overflow) × signal weight; resolution 0.1.
inline std::int64_t beta_units(double beta) { return static_cast<std::int64_t>(std::llround(beta * kSignalWeight)); }

inline Point body_centre(const Part& pt, int rot) {
  const Box& b = pt.geom[z(rot)].body;
  return Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
}

struct Move {
  int n = 0;  // parts moved
  std::array<int, kMaxMove> part{};
  std::array<Point, kMaxMove> pos{}, old_pos{};
  std::array<std::uint8_t, kMaxMove> rot{}, old_rot{};
};

// Temperature slot: the temperature and the shift-radius adaptation that belongs to it (parallel tempering
// exchanges slots between replicas, so a replica arriving at a temperature inherits its tuned radius).
struct Slot {
  double t = 1;
  double radius = 1;
  std::uint64_t tried = 0, acc = 0;
};

struct WindowStats {
  bool improved = false, proven = false;
  long nodes = 0, leaves = 0;
  std::int64_t delta = 0;
};

class Annealer {
 public:
  Annealer(const Problem& p, const std::vector<std::vector<int>>& pn, const Placement& start, const AnnealOptions& o, int stream);

  std::int64_t cost() const { return whp_ + alpha_ * cross_ + beta_ * ovf_; }
  std::int64_t crossings() const { return cross_; }
  std::int64_t overflow() const { return ovf_; }
  const Placement& current() const { return pl_; }

  // Initial temperature: mean uphill delta of random shift moves, accepted with probability p0.
  double estimate_t0(double p0);
  Coord r0() const { return r0_; }
  // Simulated annealing with a geometric schedule over `moves` steps (independent-runs mode).
  void run(std::uint64_t moves, std::chrono::steady_clock::time_point deadline, bool has_deadline);
  // `n` steps at the slot's fixed temperature (parallel tempering).
  void sweep(Slot& s, std::uint64_t n);
  // One step: an ordinary move at temperature t, or (with probability lns_rate, while LNS is on) an LNS window.
  void step(Slot& s);
  // LNS windows are greedy (kept only if better), so they only pay off in the cold part of the schedule: the
  // drivers switch them on for the last kLnsPhase of the budget.
  void set_lns(bool on) { lns_on_ = on; }

  // Large-neighbourhood search: rip up a window of parts around a seed part and re-place them (exactly by
  // branch and bound for small windows, else greedily); kept only if the cost strictly falls.
  bool lns_step();
  bool repair(std::vector<int> window);
  // Exact optimum of the window over its candidate set (branch and bound; `prune` = false enumerates every
  // combination: the reference path). Applied only if strictly better.
  WindowStats solve_window(std::vector<int> window, bool prune, long max_leaves = 200'000);
  std::vector<int> window_around(int seed, int k) const;

  void note_best();
  const Placement& best() const { return best_pl_; }
  std::int64_t best_cost() const { return best_cost_; }
  std::int64_t start_cost() const { return start_cost_; }
  std::uint64_t moves() const { return moves_; }
  std::uint64_t accepted() const { return accepted_; }
  std::uint64_t illegal() const { return illegal_; }
  std::uint64_t lns_tried() const { return lns_tried_; }
  std::uint64_t lns_improved() const { return lns_improved_; }
  bool time_limited() const { return time_limited_; }
  bool movable_empty() const { return movable_.empty(); }
  int movable_part(std::size_t i) const { return movable_[i]; }
  std::size_t movable_size() const { return movable_.size(); }
  double next() { return rng_.uniform(ctr_++); }
  std::uint64_t next_u64() { return rng_.u64(ctr_++); }

  // Move primitives (pl_ is modified by evaluate; follow with revert or commit).
  std::int64_t evaluate(const Move& m);
  bool legal(const Move& m) const;  // n ≤ 2 only
  void revert(const Move& m);
  void commit(const Move& m);

 private:
  int pick_movable() { return movable_[z(static_cast<int>(next_u64() % movable_.size()))]; }
  bool propose_shift(Move& m, Coord radius);
  bool propose_median(Move& m, Coord radius);
  bool propose_rotate(Move& m);
  bool propose_swap(Move& m);
  bool propose_swap_near(Move& m, Coord radius);
  bool single(Move& m, int a, Point q, std::uint8_t r);
  // Weighted median of the other pins of a's nets for rotation r (origin placed so the pin centroid sits there).
  bool median_target(int a, int r, Point& out) const;
  void candidates(int a, const std::vector<int>& window, const std::vector<Point>& old_pos,
                  const std::vector<std::uint8_t>& old_rot, std::vector<std::pair<Point, std::uint8_t>>& out) const;
  void restore_window(const std::vector<int>& window, const std::vector<Point>& old_pos, const std::vector<std::uint8_t>& old_rot);

  void mst_of(int n, std::vector<Seg>& out);
  void cell_range(const Seg& s, int& x0, int& y0, int& x1, int& y1) const;
  void insert_segs(int n, const std::vector<Seg>& segs);
  void remove_segs(int n);
  std::int64_t grid_cross(const Seg& s);
  static std::int64_t within(const std::vector<std::vector<Seg>>& sets);
  void touch_bin(std::size_t k);

  const Problem& p_;
  const std::vector<std::vector<int>>& pn_;
  const AnnealOptions& o_;
  Placement pl_, best_pl_;
  Legality L_;
  RngStream rng_;
  std::uint64_t ctr_ = 0;
  std::int64_t alpha_ = 0, beta_ = 0;
  std::vector<int> movable_, focus_;
  std::vector<int> group_;
  std::vector<std::vector<int>> groups_;
  std::vector<Coord> hp_;
  std::int64_t whp_ = 0, cross_ = 0, ovf_ = 0;
  Coord r0_ = 0;
  // Segment grid.
  Coord gcell_ = 0, gox_ = 0, goy_ = 0;
  int gnx_ = 1, gny_ = 1;
  std::vector<std::vector<int>> cells_;
  std::vector<Seg> pool_;
  std::vector<int> free_;
  std::vector<std::vector<int>> net_ids_;
  std::vector<std::uint32_t> stamp_;
  std::uint32_t epoch_ = 0;
  std::vector<std::uint8_t> in_c_;
  // Congestion (RUDY) state.
  CongestionMap cmap_;
  std::vector<std::int64_t> demand_, saved_;
  std::vector<Box> nbox_, new_box_;
  std::vector<std::uint32_t> bstamp_;
  std::uint32_t bepoch_ = 0;
  std::vector<std::size_t> touched_;
  std::int64_t dovf_ = 0;
  // Scratch.
  std::vector<int> nets_, cnets_, near_;
  std::vector<Coord> new_hp_;
  std::vector<std::vector<Seg>> old_segs_, new_segs_;
  std::vector<Point> pts_;
  std::int64_t dcross_ = 0, last_old_c_ = 0;
  // Results.
  std::int64_t best_cost_ = 0, start_cost_ = 0;
  std::uint64_t moves_ = 0, accepted_ = 0, illegal_ = 0, lns_tried_ = 0, lns_improved_ = 0;
  bool time_limited_ = false;
  bool lns_on_ = true;
};

// Parallel-tempering driver (tempering.cpp).
AnnealResult anneal_tempering(const Problem& p, const std::vector<std::vector<int>>& pn, const Placement& start, const AnnealOptions& o,
                              std::uint64_t moves);
// Fills the result fields shared by both drivers from the winning replica.
void finish_result(const Problem& p, const AnnealOptions& o, AnnealResult& res);

}  // namespace tmk::place::detail
