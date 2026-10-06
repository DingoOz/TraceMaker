// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/global_router.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>

#include "route/obstacles.hpp"

namespace tmk::route {
namespace {

struct Graph {
  int nx = 0, ny = 0, nl = 0;
  // Horizontal edges (tx,ty)-(tx+1,ty) and vertical edges (tx,ty)-(tx,ty+1) per layer.
  std::vector<int> cap_h, cap_v, use_h, use_v;
  std::vector<std::int64_t> hist_h, hist_v;
  std::size_t X() const { return static_cast<std::size_t>(nx); }
  std::size_t Y() const { return static_cast<std::size_t>(ny); }
  static std::size_t z(int v) { return static_cast<std::size_t>(v); }
  std::size_t hi(int l, int x, int y) const { return (z(l) * Y() + z(y)) * (X() - 1) + z(x); }
  std::size_t vi(int l, int x, int y) const { return (z(l) * (Y() - 1) + z(y)) * X() + z(x); }
  std::size_t node(int l, int x, int y) const { return (z(l) * Y() + z(y)) * X() + z(x); }
};

constexpr std::int64_t kUnit = 256;  // cost of one tile step
std::size_t z2(const Graph& g, int x, int y) { return static_cast<std::size_t>(y) * g.X() + static_cast<std::size_t>(x); }

}  // namespace

GlobalResult global_route(const Obstacles& obs, const geom::Box& bounds, int layers, const std::vector<GlobalNet>& nets,
                          const GlobalOptions& opt) {
  GlobalResult res;
  const auto t_start = std::chrono::steady_clock::now();
  const Coord pitch = std::max<Coord>(opt.pitch, 100'000);
  const Coord tile = opt.tile > 0 ? opt.tile : std::clamp<Coord>(8 * pitch, 1'500'000, 5'000'000);
  res.tile = tile;
  res.origin = {bounds.x0, bounds.y0};
  Graph g;
  g.nx = std::max(2, static_cast<int>((bounds.x1 - bounds.x0) / tile) + 1);
  g.ny = std::max(2, static_cast<int>((bounds.y1 - bounds.y0) / tile) + 1);
  g.nl = layers;
  res.tiles_x = g.nx;
  res.tiles_y = g.ny;
  res.layers = layers;
  g.cap_h.assign(static_cast<std::size_t>(layers) * g.Y() * (g.X() - 1), 0);
  g.cap_v.assign(static_cast<std::size_t>(layers) * (g.Y() - 1) * g.X(), 0);
  g.use_h.assign(g.cap_h.size(), 0);
  g.use_v.assign(g.cap_v.size(), 0);
  g.hist_h.assign(g.cap_h.size(), 0);
  g.hist_v.assign(g.cap_v.size(), 0);

  // Capacities: tracks that fit across the narrowest of eight cut lines between the two tile centres. A cut is
  // sampled at quarter-pitch steps; a free run of n samples holds 1 + (n - 1) * step / pitch tracks, so gaps
  // narrower than a pitch (between the pins of a row) count once and blocked stretches not at all. The boundary
  // alone overestimates badly: on a through-hole board it often falls between pin rows and looks empty.
  const Coord hw = pitch / 4;  // a narrow probe: capacity counts free cross-section, the pitch divides it
  const Coord step = std::max<Coord>(pitch / 4, 25'000);
  constexpr int kCuts = 8;
  auto free_at = [&](geom::Point p, int l) { return obs.fixed_code(p, l, hw, 0, 0) == Obstacles::kFree; };
  auto cut_capacity = [&](Coord fixed, Coord from, bool vertical_line, int l) {
    int cap = 0, run = 0;
    auto close = [&] {
      if (run > 0) cap += 1 + static_cast<int>(static_cast<Coord>(run - 1) * step / pitch);
      run = 0;
    };
    for (Coord t = step / 2; t < tile; t += step) {
      if (free_at(vertical_line ? geom::Point{fixed, from + t} : geom::Point{from + t, fixed}, l)) ++run;
      else close();
    }
    close();
    return cap;
  };
  // line_x[x * kCuts + k]: tracks across the whole straight line x = const (cut k between tile columns x and x + 1),
  // all rows and layers; line_y likewise. Unlike the per-tile minimum these are capacities of real lines (R6).
  std::vector<long> line_x(g.X() * kCuts, 0), line_y(g.Y() * kCuts, 0);
  for (int l = 0; l < layers; ++l)
    for (int y = 0; y < g.ny; ++y)
      for (int x = 0; x < g.nx; ++x) {
        const Coord x0 = bounds.x0 + x * tile, y0 = bounds.y0 + y * tile;
        if (x + 1 < g.nx) {  // cuts x = const between the centres of tiles x and x + 1, from y0 to y0 + tile
          int c = std::numeric_limits<int>::max();
          for (int k = 0; k < kCuts; ++k) {
            const int ck = cut_capacity(x0 + tile / 2 + (2 * k + 1) * tile / (2 * kCuts), y0, true, l);
            line_x[static_cast<std::size_t>(x) * kCuts + static_cast<std::size_t>(k)] += ck;
            c = std::min(c, ck);
          }
          g.cap_h[g.hi(l, x, y)] = c;
        }
        if (y + 1 < g.ny) {
          int c = std::numeric_limits<int>::max();
          for (int k = 0; k < kCuts; ++k) {
            const int ck = cut_capacity(y0 + tile / 2 + (2 * k + 1) * tile / (2 * kCuts), x0, false, l);
            line_y[static_cast<std::size_t>(y) * kCuts + static_cast<std::size_t>(k)] += ck;
            c = std::min(c, ck);
          }
          g.cap_v[g.vi(l, x, y)] = c;
        }
      }
  // Cut lines (R6): nets with endpoints on both sides of each line against the line's capacity.
  {
    // Per net: is there a planned connection across the line, and does a pad of the net straddle it?
    std::vector<int> ids;
    for (const auto& n : nets)
      if (n.net > 0) ids.push_back(n.net);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    auto demand_at = [&](bool vertical, Coord at) {
      auto lo = [&](const geom::Box& bx) { return vertical ? bx.x0 : bx.y0; };
      auto hi = [&](const geom::Box& bx) { return vertical ? bx.x1 : bx.y1; };
      std::vector<int> need, bridged;
      for (const auto& n : nets)
        if (n.net > 0 && ((hi(n.box_a) < at && lo(n.box_b) > at) || (hi(n.box_b) < at && lo(n.box_a) > at))) need.push_back(n.net);
      for (const auto& [net, bx] : opt.pads)
        if (lo(bx) <= at && at <= hi(bx)) bridged.push_back(net);
      std::sort(need.begin(), need.end());
      need.erase(std::unique(need.begin(), need.end()), need.end());
      std::sort(bridged.begin(), bridged.end());
      int d = 0;
      for (int net : need) d += !std::binary_search(bridged.begin(), bridged.end(), net);
      return d;
    };
    bool have = false;
    auto consider = [&](bool vertical, Coord at, long cap, std::vector<CutLine>& over_here) {
      CutLine c{vertical, at, static_cast<int>(std::min<long>(cap, std::numeric_limits<int>::max())), 0};
      c.demand = demand_at(vertical, at);
      if (c.demand == 0) return;
      // Tighter: demand / capacity larger, compared by cross-multiplication.
      auto tighter = [](const CutLine& p, const CutLine& q) { return static_cast<long>(p.demand) * q.capacity > static_cast<long>(q.demand) * p.capacity; };
      if (!have || tighter(c, res.tightest)) res.tightest = c, have = true;
      if (c.demand > c.capacity && (over_here.empty() || tighter(c, over_here.front()))) over_here.assign(1, c);
    };
    for (int x = 0; x + 1 < g.nx; ++x) {
      std::vector<CutLine> over_here;
      for (int k = 0; k < kCuts; ++k) consider(true, bounds.x0 + x * tile + tile / 2 + (2 * k + 1) * tile / (2 * kCuts), line_x[static_cast<std::size_t>(x) * kCuts + static_cast<std::size_t>(k)], over_here);
      res.over_cuts.insert(res.over_cuts.end(), over_here.begin(), over_here.end());
    }
    for (int y = 0; y + 1 < g.ny; ++y) {
      std::vector<CutLine> over_here;
      for (int k = 0; k < kCuts; ++k) consider(false, bounds.y0 + y * tile + tile / 2 + (2 * k + 1) * tile / (2 * kCuts), line_y[static_cast<std::size_t>(y) * kCuts + static_cast<std::size_t>(k)], over_here);
      res.over_cuts.insert(res.over_cuts.end(), over_here.begin(), over_here.end());
    }
  }

  // Via capacity: free through-via sites on a grid of via pitches inside the tile (fixed copper only).
  std::vector<int> via_cap(g.X() * g.Y(), std::numeric_limits<int>::max()), via_use(g.X() * g.Y(), 0);
  std::vector<std::int64_t> via_hist(g.X() * g.Y(), 0);
  // At most 8 x 8 sites a tile: boards without via rules (old formats) would otherwise give a pitch of microns.
  const Coord vp = std::max({opt.via_pitch, tile / 8, Coord{300'000}});
  if (opt.via_diameter > 0 && opt.via_pitch > 0)
    for (int y = 0; y < g.ny; ++y)
      for (int x = 0; x < g.nx; ++x) {
        int sites = 0;
        for (Coord vy = vp / 2; vy < tile; vy += vp)
          for (Coord vx = vp / 2; vx < tile; vx += vp)
            sites += obs.fixed_via_code({bounds.x0 + x * tile + vx, bounds.y0 + y * tile + vy}, opt.via_diameter, opt.via_drill, 0, 0) == Obstacles::kFree;
        via_cap[z2(g, x, y)] = sites;
      }
  const auto t_cap = std::chrono::steady_clock::now();
  res.seconds_capacity = std::chrono::duration<double>(t_cap - t_start).count();

  auto tx_of = [&](Coord x) { return std::clamp(static_cast<int>((x - bounds.x0) / tile), 0, g.nx - 1); };
  auto ty_of = [&](Coord y) { return std::clamp(static_cast<int>((y - bounds.y0) / tile), 0, g.ny - 1); };
  // Integer costs (rule 2): kUnit per tile step.
  const std::int64_t via_cost = static_cast<std::int64_t>(opt.via_cost_tiles * static_cast<double>(kUnit));
  std::int64_t pres = kUnit;  // present-congestion weight, grows each round
  // Nets on each edge: (net, connections of it using the edge). Demand = distinct nets.
  std::vector<std::vector<std::pair<int, int>>> on_h(g.cap_h.size()), on_v(g.cap_v.size());
  auto holds = [](const std::vector<std::pair<int, int>>& on, int net) {
    if (net <= 0) return false;
    for (const auto& [n, c] : on)
      if (n == net) return true;
    return false;
  };
  auto edge_cost = [&](int cap, int use, std::int64_t hist, bool own) {
    if (own) return kUnit;  // the net's own trunk: no new demand
    std::int64_t c = kUnit + hist;
    if (use + 1 > cap) c += pres * (use + 1 - cap) * 4;
    return c;
  };

  // A path is its node sequence, start to goal.
  std::vector<std::vector<std::size_t>> paths(nets.size());
  const std::size_t plane = g.X() * g.Y();
  auto step_edge = [&](std::size_t u, std::size_t v, int& kind) -> std::size_t {
    const int lu = static_cast<int>(u / plane), lv = static_cast<int>(v / plane);
    const int xu = static_cast<int>(u % g.X()), yu = static_cast<int>((u / g.X()) % g.Y());
    const int xv = static_cast<int>(v % g.X()), yv = static_cast<int>((v / g.X()) % g.Y());
    if (lu != lv) { kind = 2; return z2(g, xu, yu); }
    if (yu == yv) { kind = 0; return g.hi(lu, std::min(xu, xv), yu); }
    kind = 1;
    return g.vi(lu, xu, std::min(yu, yv));
  };
  auto apply = [&](std::size_t k, int d) {
    const int net = nets[k].net;
    for (std::size_t i = 0; i + 1 < paths[k].size(); ++i) {
      int kind = 0;
      const std::size_t e = step_edge(paths[k][i], paths[k][i + 1], kind);
      if (kind == 2) {
        via_use[e] += d;
        continue;
      }
      auto& on = (kind == 0 ? on_h : on_v)[e];
      auto& use = (kind == 0 ? g.use_h : g.use_v)[e];
      // Net 0 shares with nothing: adding always makes a new entry, removing takes any one of its entries.
      auto it = std::find_if(on.begin(), on.end(), [&](const auto& pr) { return pr.first == net && (net > 0 || d < 0); });
      if (it == on.end()) {
        on.emplace_back(net, 1);  // d is +1 here: a path is never removed before it was added
        ++use;
      } else if ((it->second += d) == 0) {
        on.erase(it);
        --use;
      }
    }
  };
  auto over = [&](std::size_t k) {
    for (std::size_t i = 0; i + 1 < paths[k].size(); ++i) {
      int kind = 0;
      const std::size_t e = step_edge(paths[k][i], paths[k][i + 1], kind);
      if (kind == 0 ? g.use_h[e] > g.cap_h[e] : kind == 1 ? g.use_v[e] > g.cap_v[e] : via_use[e] > via_cap[e]) return true;
    }
    return false;
  };
  const std::size_t nn = static_cast<std::size_t>(layers) * plane;
  constexpr std::int64_t kInf = std::numeric_limits<std::int64_t>::max();
  std::vector<std::int64_t> dist(nn), from(nn);
  auto route_one = [&](std::size_t k) {
    const auto& n = nets[k];
    const int ax = tx_of(n.a.x), ay = ty_of(n.a.y), bx = tx_of(n.b.x), by = ty_of(n.b.y);
    std::fill(dist.begin(), dist.end(), kInf);
    std::fill(from.begin(), from.end(), -1);
    using QE = std::pair<std::int64_t, std::size_t>;  // (f, node): ties resolve by node index, deterministically
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    auto h = [&](int x, int y) { return static_cast<std::int64_t>(std::abs(x - bx) + std::abs(y - by)) * kUnit; };
    for (int l = 0; l < layers; ++l)
      if (n.layers_a & model::layer_bit(l)) {
        const std::size_t s = g.node(l, ax, ay);
        dist[s] = 0;
        pq.emplace(h(ax, ay), s);
      }
    std::size_t goal = SIZE_MAX;
    while (!pq.empty()) {
      const auto [f, u] = pq.top();
      pq.pop();
      const int l = static_cast<int>(u / plane);
      const int y = static_cast<int>((u / g.X()) % g.Y()), x = static_cast<int>(u % g.X());
      if (f - h(x, y) > dist[u]) continue;
      if (x == bx && y == by && (n.layers_b & model::layer_bit(l))) {
        goal = u;
        break;
      }
      auto relax = [&](std::size_t v, std::int64_t c, int vx, int vy) {
        const std::int64_t nd = dist[u] + c;
        if (nd < dist[v]) {
          dist[v] = nd;
          from[v] = static_cast<std::int64_t>(u);
          pq.emplace(nd + h(vx, vy), v);
        }
      };
      auto horiz = [&](std::size_t e, int vx) { relax(g.node(l, vx, y), edge_cost(g.cap_h[e], g.use_h[e], g.hist_h[e], holds(on_h[e], n.net)), vx, y); };
      auto vert = [&](std::size_t e, int vy) { relax(g.node(l, x, vy), edge_cost(g.cap_v[e], g.use_v[e], g.hist_v[e], holds(on_v[e], n.net)), x, vy); };
      if (x + 1 < g.nx) horiz(g.hi(l, x, y), x + 1);
      if (x > 0) horiz(g.hi(l, x - 1, y), x - 1);
      if (y + 1 < g.ny) vert(g.vi(l, x, y), y + 1);
      if (y > 0) vert(g.vi(l, x, y - 1), y - 1);
      const std::size_t t = z2(g, x, y);
      std::int64_t vc = via_hist[t];
      if (via_use[t] + 1 > via_cap[t]) vc += pres * (via_use[t] + 1 - via_cap[t]) * 4;
      for (int l2 = 0; l2 < layers; ++l2)
        if (l2 != l) relax(g.node(l2, x, y), via_cost * std::abs(l2 - l) + vc, x, y);
    }
    paths[k].clear();
    if (goal == SIZE_MAX) return;
    for (std::int64_t v = static_cast<std::int64_t>(goal); v >= 0; v = from[static_cast<std::size_t>(v)]) paths[k].push_back(static_cast<std::size_t>(v));
    std::reverse(paths[k].begin(), paths[k].end());
  };

  // First pass, shortest connections first (deterministic order).
  std::vector<std::size_t> order(nets.size());
  for (std::size_t k = 0; k < nets.size(); ++k) order[k] = k;
  auto len = [&](std::size_t k) { return std::llabs(nets[k].a.x - nets[k].b.x) + std::llabs(nets[k].a.y - nets[k].b.y); };
  std::stable_sort(order.begin(), order.end(), [&](std::size_t p, std::size_t q) { return len(p) < len(q); });
  for (std::size_t k : order) {
    route_one(k);
    apply(k, +1);
  }
  // Negotiation: rip up connections that use an overflowed edge or via tile, raise history there, re-route.
  for (int it = 0; it < opt.iterations; ++it) {
    for (std::size_t e = 0; e < g.use_h.size(); ++e)
      if (g.use_h[e] > g.cap_h[e]) g.hist_h[e] += kUnit / 2 * (g.use_h[e] - g.cap_h[e]);
    for (std::size_t e = 0; e < g.use_v.size(); ++e)
      if (g.use_v[e] > g.cap_v[e]) g.hist_v[e] += kUnit / 2 * (g.use_v[e] - g.cap_v[e]);
    for (std::size_t t = 0; t < via_use.size(); ++t)
      if (via_use[t] > via_cap[t]) via_hist[t] += kUnit / 2 * (via_use[t] - via_cap[t]);
    pres = pres * 8 / 5;
    bool any = false;
    for (std::size_t k : order) {
      if (!over(k)) continue;
      any = true;
      apply(k, -1);
      route_one(k);
      apply(k, +1);
    }
    if (!any) break;
  }
  for (std::size_t e = 0; e < g.use_h.size(); ++e)
    if (g.use_h[e] > g.cap_h[e]) { ++res.overflow_edges; res.total_overflow += g.use_h[e] - g.cap_h[e]; }
  for (std::size_t e = 0; e < g.use_v.size(); ++e)
    if (g.use_v[e] > g.cap_v[e]) { ++res.overflow_edges; res.total_overflow += g.use_v[e] - g.cap_v[e]; }
  for (std::size_t t = 0; t < via_use.size(); ++t) res.overflow_via_tiles += via_use[t] > via_cap[t];

  // Congestion map: demand over capacity on the four boundaries of each tile (integers, so every platform agrees).
  res.util.assign(nn, 0);
  for (int l = 0; l < layers; ++l)
    for (int y = 0; y < g.ny; ++y)
      for (int x = 0; x < g.nx; ++x) {
        long use = 0, cap = 0;
        auto add = [&](const std::vector<int>& u, const std::vector<int>& c, std::size_t e) { use += u[e]; cap += c[e]; };
        if (x + 1 < g.nx) add(g.use_h, g.cap_h, g.hi(l, x, y));
        if (x > 0) add(g.use_h, g.cap_h, g.hi(l, x - 1, y));
        if (y + 1 < g.ny) add(g.use_v, g.cap_v, g.vi(l, x, y));
        if (y > 0) add(g.use_v, g.cap_v, g.vi(l, x, y - 1));
        res.util[g.node(l, x, y)] = static_cast<std::uint8_t>(std::min<long>(255, use * 8 / std::max<long>(1, cap)));
      }

  // Corridors: each path's tiles on the layers it uses, widened by one tile. Both end tiles are marked on all
  // of their pad's layers. A via marks its tile on the two layers it joins only (no via columns).
  res.corridor.resize(nets.size());
  res.corridor_box.assign(nets.size(), {0, 0, g.nx - 1, g.ny - 1});
  res.vias.assign(nets.size(), 0);
  for (std::size_t k = 0; k < nets.size(); ++k) {
    auto& c = res.corridor[k];
    c.assign(nn, 0);
    auto mark = [&](int l, int x, int y) {
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int xx = x + dx, yy = y + dy;
          if (xx >= 0 && yy >= 0 && xx < g.nx && yy < g.ny) c[g.node(l, xx, yy)] = 1;
        }
    };
    const auto& n = nets[k];
    for (int l = 0; l < layers; ++l) {
      if (n.layers_a & model::layer_bit(l)) mark(l, tx_of(n.a.x), ty_of(n.a.y));
      if (n.layers_b & model::layer_bit(l)) mark(l, tx_of(n.b.x), ty_of(n.b.y));
    }
    if (paths[k].empty()) {  // not routed globally: no guidance (the whole board is the corridor)
      std::fill(c.begin(), c.end(), 1);
      continue;
    }
    for (std::size_t i = 0; i < paths[k].size(); ++i) {
      const std::size_t u = paths[k][i];
      mark(static_cast<int>(u / plane), static_cast<int>(u % g.X()), static_cast<int>((u / g.X()) % g.Y()));
      if (i > 0 && u / plane != paths[k][i - 1] / plane) ++res.vias[k];
    }
  }
  for (std::size_t k = 0; k < nets.size(); ++k) {
    std::array<int, 4> bx{g.nx, g.ny, -1, -1};
    const auto& c = res.corridor[k];
    for (int l = 0; l < layers; ++l)
      for (int y = 0; y < g.ny; ++y)
        for (int x = 0; x < g.nx; ++x)
          if (c[g.node(l, x, y)]) bx = {std::min(bx[0], x), std::min(bx[1], y), std::max(bx[2], x), std::max(bx[3], y)};
    if (bx[2] >= 0) res.corridor_box[k] = bx;
  }
  res.seconds_route = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_cap).count();
  return res;
}

}  // namespace tmk::route
