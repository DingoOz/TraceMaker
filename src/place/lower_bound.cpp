// SPDX-License-Identifier: GPL-3.0-or-later
#include "place/lower_bound.hpp"

#include <algorithm>
#include <climits>
#include <queue>

namespace tmk::place {

void MinCostFlow::add_arc(int u, int v, std::int64_t cost, std::int64_t cap) {
  arcs_.push_back(Arc{v, head_[z(u)], cap, cost});
  head_[z(u)] = static_cast<int>(arcs_.size()) - 1;
  arcs_.push_back(Arc{u, head_[z(v)], 0, -cost});
  head_[z(v)] = static_cast<int>(arcs_.size()) - 1;
}

std::pair<std::int64_t, std::int64_t> MinCostFlow::solve(int s, int t, std::int64_t limit) {
  const std::size_t n = head_.size();
  constexpr std::int64_t inf = INT64_MAX / 4;
  // Bellman–Ford potentials (arc costs may be negative; there is no negative cycle).
  std::vector<std::int64_t> pot(n, inf);
  pot[z(s)] = 0;
  for (std::size_t it = 0; it < n; ++it) {
    bool changed = false;
    for (std::size_t u = 0; u < n; ++u) {
      if (pot[u] == inf) continue;
      for (int e = head_[u]; e >= 0; e = arcs_[z(e)].next) {
        const Arc& a = arcs_[z(e)];
        if (a.cap > 0 && pot[u] + a.cost < pot[z(a.to)]) {
          pot[z(a.to)] = pot[u] + a.cost;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }
  for (auto& v : pot)
    if (v == inf) v = 0;
  std::int64_t flow = 0, cost = 0;
  std::vector<std::int64_t> dist(n);
  std::vector<int> prev(n);
  using QE = std::pair<std::int64_t, int>;
  while (flow < limit) {
    std::fill(dist.begin(), dist.end(), inf);
    std::fill(prev.begin(), prev.end(), -1);
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    dist[z(s)] = 0;
    pq.emplace(0, s);
    while (!pq.empty()) {
      const auto [d, u] = pq.top();
      pq.pop();
      if (d != dist[z(u)]) continue;
      for (int e = head_[z(u)]; e >= 0; e = arcs_[z(e)].next) {
        const Arc& a = arcs_[z(e)];
        if (a.cap <= 0) continue;
        const std::int64_t nd = d + a.cost + pot[z(u)] - pot[z(a.to)];
        if (nd < dist[z(a.to)]) {
          dist[z(a.to)] = nd;
          prev[z(a.to)] = e;
          pq.emplace(nd, a.to);
        }
      }
    }
    if (dist[z(t)] == inf) break;
    for (std::size_t v = 0; v < n; ++v)
      if (dist[v] < inf) pot[v] += dist[v];
    std::int64_t push = limit - flow;
    for (int v = t; v != s; v = arcs_[z(prev[z(v)] ^ 1)].to) push = std::min(push, arcs_[z(prev[z(v)])].cap);
    for (int v = t; v != s; v = arcs_[z(prev[z(v)] ^ 1)].to) {
      arcs_[z(prev[z(v)])].cap -= push;
      arcs_[z(prev[z(v)] ^ 1)].cap += push;
      cost += push * arcs_[z(prev[z(v)])].cost;
    }
    flow += push;
  }
  return {flow, cost};
}

namespace {

std::int64_t axis_bound(const Problem& p, const Placement& pl, RotationModel model, int axis) {
  const int nn = static_cast<int>(p.nets.size());
  // Node ids: S, T, U_n, L_n, Z (ground), X_i per movable part.
  const int S = 0, T = 1, Z = 2 + 2 * nn;
  std::vector<int> xnode(p.parts.size(), -1);
  int next = Z + 1;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable) xnode[i] = next++;
  MinCostFlow g(next);
  auto coord = [axis](Point q) { return axis == 0 ? q.x : q.y; };
  std::int64_t total = 0;
  for (int n = 0; n < nn; ++n) {
    const PNet& net = p.nets[z(n)];
    const int U = 2 + n, L = 2 + nn + n;
    g.add_arc(S, U, 0, net.weight);
    g.add_arc(L, T, 0, net.weight);
    g.add_arc(U, L, 0);  // U − L ≥ 0 (implied for fixed rotations; needed when pins choose their offsets freely)
    total += net.weight;
    // Fixed pins: absolute coordinates through the ground node.
    Coord fmax = LLONG_MIN, fmin = LLONG_MAX;
    // Movable parts: per part, the binding offsets.
    std::vector<int> parts;
    for (int pi : net.pins) {
      const Pin& q = p.pins[z(pi)];
      if (!p.parts[z(q.part)].movable) {
        const Coord c = coord(pl.pin(p, pi));
        fmax = std::max(fmax, c);
        fmin = std::min(fmin, c);
      } else if (std::find(parts.begin(), parts.end(), q.part) == parts.end()) {
        parts.push_back(q.part);
      }
    }
    if (fmax != LLONG_MIN) {
      // U − Z ≥ fmax  and  Z − L ≥ −fmin ; arc cost = −c (we maximise Σ c·f).
      g.add_arc(U, Z, -fmax);
      g.add_arc(Z, L, fmin);
    }
    for (int part : parts) {
      // For rotation r: U − X ≥ max_k o_k(r) and X − L ≥ −min_k o_k(r).
      Coord up = LLONG_MAX, lo = LLONG_MIN;
      const int states = p.parts[z(part)].may_flip() ? kStates : 4;  // either side when the part may flip
      for (int r = 0; r < states; ++r) {
        if (model == RotationModel::Fixed && r != pl.rot[z(part)]) continue;
        Coord mx = LLONG_MIN, mn = LLONG_MAX;
        for (int pi : net.pins) {
          const Pin& q = p.pins[z(pi)];
          if (q.part != part) continue;
          mx = std::max(mx, coord(q.off[z(r)]));
          mn = std::min(mn, coord(q.off[z(r)]));
        }
        up = std::min(up, mx);  // weakest (smallest) upper requirement over rotations
        lo = std::max(lo, mn);  // weakest (largest) lower requirement over rotations
      }
      g.add_arc(U, xnode[z(part)], -up);
      g.add_arc(xnode[z(part)], L, lo);
    }
  }
  const auto [flow, cost] = g.solve(S, T, total);
  (void)flow;  // always the full supply: every net has a path U_n → (part or ground) → L_n
  return -cost;
}

}  // namespace

std::int64_t hpwl_lower_bound(const Problem& p, const Placement& pl, RotationModel model) {
  return axis_bound(p, pl, model, 0) + axis_bound(p, pl, model, 1);
}

}  // namespace tmk::place
