#pragma once
// Exact lower bound on weighted HPWL (design doc 04 §1 level L4).
//
// Without overlap and outline constraints, minimising Σ_n w_n (max_i x_i − min_i x_i) over the movable
// parts' positions is a linear program whose constraints are all differences (U_n − X_p ≥ o, X_p − L_n ≥ −o),
// so its dual is an uncapacitated min-cost flow from the nets' upper nodes U_n through the parts (or the fixed
// "ground" node) to the nets' lower nodes L_n. Solved exactly in integers by successive shortest paths
// (Ahuja, Magnanti, Orlin, "Network Flows", 1993, ch. 9). By LP duality the optimum equals the minimum of the
// relaxed placement problem, so it bounds every legal placement from below.
#include <cstdint>

#include "place/legality.hpp"

namespace tmk::place {

enum class RotationModel {
  Fixed,  // movable parts keep the rotations in the given placement: the exact relaxed optimum for them
  Any,    // valid for every rotation of the movable parts (each pin may take its most favourable offset
          // per axis independently): a weaker bound that also holds for our final placement
};

// Minimum weighted HPWL (Σ weight · HPWL, nm) over all positions of the movable parts with overlaps and the
// board outline ignored. Fixed parts are taken from `pl`.
std::int64_t hpwl_lower_bound(const Problem& p, const Placement& pl, RotationModel model);

// Min-cost flow on a small graph (exposed for tests). Successive shortest paths with Dijkstra and
// potentials; arcs from add_arc have infinite capacity unless `cap` is given. Costs are integers.
class MinCostFlow {
 public:
  explicit MinCostFlow(int n) : head_(static_cast<std::size_t>(n), -1) {}
  static constexpr std::int64_t kInf = INT64_MAX / 4;
  void add_arc(int u, int v, std::int64_t cost, std::int64_t cap = kInf);
  // Sends up to `limit` units from s to t at minimum cost; returns {flow, cost}. The graph must have no
  // negative cycle; negative arc costs are allowed (initial potentials by Bellman–Ford).
  std::pair<std::int64_t, std::int64_t> solve(int s, int t, std::int64_t limit);

 private:
  struct Arc {
    int to, next;
    std::int64_t cap, cost;
  };
  std::vector<int> head_;
  std::vector<Arc> arcs_;
};

}  // namespace tmk::place
