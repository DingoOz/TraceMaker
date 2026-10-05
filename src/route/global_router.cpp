#include "route/global_router.hpp"

#include <algorithm>
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
  std::vector<double> hist_h, hist_v;
  std::size_t X() const { return static_cast<std::size_t>(nx); }
  std::size_t Y() const { return static_cast<std::size_t>(ny); }
  static std::size_t z(int v) { return static_cast<std::size_t>(v); }
  std::size_t hi(int l, int x, int y) const { return (z(l) * Y() + z(y)) * (X() - 1) + z(x); }
  std::size_t vi(int l, int x, int y) const { return (z(l) * (Y() - 1) + z(y)) * X() + z(x); }
  std::size_t node(int l, int x, int y) const { return (z(l) * Y() + z(y)) * X() + z(x); }
};

// One step of a path: from node to node, through a horizontal (0) / vertical (1) edge or a via (2).
struct Step { int kind; std::size_t edge; };

}  // namespace

GlobalResult global_route(const Obstacles& obs, const geom::Box& bounds, int layers, const std::vector<GlobalNet>& nets,
                          const GlobalOptions& opt) {
  GlobalResult res;
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
  g.hist_h.assign(g.cap_h.size(), 0.0);
  g.hist_v.assign(g.cap_v.size(), 0.0);

  // Capacities: sample the shared boundary of two tiles at half-pitch steps; free samples x step / pitch tracks.
  const Coord hw = pitch / 4;  // a narrow probe: capacity counts free cross-section, the pitch divides it
  const Coord step = std::max<Coord>(pitch / 2, 25'000);
  auto free_at = [&](geom::Point p, int l) { return obs.fixed_code(p, l, hw, 0, 0) == Obstacles::kFree; };
  for (int l = 0; l < layers; ++l)
    for (int y = 0; y < g.ny; ++y)
      for (int x = 0; x < g.nx; ++x) {
        const Coord x0 = bounds.x0 + x * tile, y0 = bounds.y0 + y * tile;
        if (x + 1 < g.nx) {  // boundary x = x0 + tile, from y0 to y0 + tile
          int f = 0;
          for (Coord t = step / 2; t < tile; t += step) f += free_at({x0 + tile, y0 + t}, l);
          g.cap_h[g.hi(l, x, y)] = static_cast<int>(static_cast<double>(f) * static_cast<double>(step) / static_cast<double>(pitch));
        }
        if (y + 1 < g.ny) {
          int f = 0;
          for (Coord t = step / 2; t < tile; t += step) f += free_at({x0 + t, y0 + tile}, l);
          g.cap_v[g.vi(l, x, y)] = static_cast<int>(static_cast<double>(f) * static_cast<double>(step) / static_cast<double>(pitch));
        }
      }

  auto tx_of = [&](Coord x) { return std::clamp(static_cast<int>((x - bounds.x0) / tile), 0, g.nx - 1); };
  auto ty_of = [&](Coord y) { return std::clamp(static_cast<int>((y - bounds.y0) / tile), 0, g.ny - 1); };
  const double via_cost = opt.via_cost_tiles;
  double pres = 1.0;  // present-congestion weight, grows each round
  auto edge_cost = [&](int cap, int use, double hist) {
    double c = 1.0 + hist;
    if (use + 1 > cap) c += pres * (use + 1 - cap) * 4.0;
    return c;
  };

  std::vector<std::vector<Step>> paths(nets.size());
  std::vector<std::size_t> start_node(nets.size(), 0);
  auto apply = [&](std::size_t k, int d) {
    for (const auto& s : paths[k]) {
      if (s.kind == 0) g.use_h[s.edge] += d;
      else if (s.kind == 1) g.use_v[s.edge] += d;
    }
  };
  const std::size_t nn = static_cast<std::size_t>(layers) * g.X() * g.Y();
  std::vector<double> dist(nn);
  std::vector<std::int64_t> from(nn);
  std::vector<int> from_kind(nn);
  std::vector<std::size_t> from_edge(nn);
  auto route_one = [&](std::size_t k) {
    const auto& n = nets[k];
    const int ax = tx_of(n.a.x), ay = ty_of(n.a.y), bx = tx_of(n.b.x), by = ty_of(n.b.y);
    std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
    std::fill(from.begin(), from.end(), -1);
    using QE = std::pair<double, std::size_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    auto h = [&](int x, int y) { return static_cast<double>(std::abs(x - bx) + std::abs(y - by)); };
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
      const int l = static_cast<int>(u / (g.X() * g.Y()));
      const int y = static_cast<int>((u / g.X()) % g.Y()), x = static_cast<int>(u % g.X());
      if (f - h(x, y) > dist[u] + 1e-9) continue;
      if (x == bx && y == by && (n.layers_b & model::layer_bit(l))) {
        goal = u;
        break;
      }
      auto relax = [&](std::size_t v, double c, int kind, std::size_t edge, int vx, int vy) {
        const double nd = dist[u] + c;
        if (nd < dist[v]) {
          dist[v] = nd;
          from[v] = static_cast<std::int64_t>(u);
          from_kind[v] = kind;
          from_edge[v] = edge;
          pq.emplace(nd + h(vx, vy), v);
        }
      };
      if (x + 1 < g.nx) { const auto e = g.hi(l, x, y); relax(g.node(l, x + 1, y), edge_cost(g.cap_h[e], g.use_h[e], g.hist_h[e]), 0, e, x + 1, y); }
      if (x > 0) { const auto e = g.hi(l, x - 1, y); relax(g.node(l, x - 1, y), edge_cost(g.cap_h[e], g.use_h[e], g.hist_h[e]), 0, e, x - 1, y); }
      if (y + 1 < g.ny) { const auto e = g.vi(l, x, y); relax(g.node(l, x, y + 1), edge_cost(g.cap_v[e], g.use_v[e], g.hist_v[e]), 1, e, x, y + 1); }
      if (y > 0) { const auto e = g.vi(l, x, y - 1); relax(g.node(l, x, y - 1), edge_cost(g.cap_v[e], g.use_v[e], g.hist_v[e]), 1, e, x, y - 1); }
      for (int l2 = 0; l2 < layers; ++l2)
        if (l2 != l) relax(g.node(l2, x, y), via_cost * std::abs(l2 - l), 2, 0, x, y);
    }
    paths[k].clear();
    if (goal == SIZE_MAX) return;
    for (std::size_t v = goal; from[v] >= 0; v = static_cast<std::size_t>(from[v])) paths[k].push_back({from_kind[v], from_edge[v]});
    start_node[k] = goal;
    // Keep the node sequence too, for corridors: rebuild by walking again.
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
  // Negotiation: rip up connections that use an overflowed edge, raise history there, re-route.
  for (int it = 0; it < opt.iterations; ++it) {
    for (std::size_t e = 0; e < g.use_h.size(); ++e)
      if (g.use_h[e] > g.cap_h[e]) g.hist_h[e] += 0.5 * (g.use_h[e] - g.cap_h[e]);
    for (std::size_t e = 0; e < g.use_v.size(); ++e)
      if (g.use_v[e] > g.cap_v[e]) g.hist_v[e] += 0.5 * (g.use_v[e] - g.cap_v[e]);
    pres *= 1.6;
    bool any = false;
    for (std::size_t k : order) {
      bool over = false;
      for (const auto& s : paths[k])
        if ((s.kind == 0 && g.use_h[s.edge] > g.cap_h[s.edge]) || (s.kind == 1 && g.use_v[s.edge] > g.cap_v[s.edge])) over = true;
      if (!over) continue;
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

  // Corridors: walk each path's nodes (re-derived from the edges, starting at the goal node), mark tiles, widen
  // by one tile on the same layer. Both end tiles are marked on all of their pad's layers.
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
    // Walk back from the goal: undo each step to recover the node sequence.
    std::size_t u = start_node[k];
    int l = static_cast<int>(u / (g.X() * g.Y()));
    int y = static_cast<int>((u / g.X()) % g.Y()), x = static_cast<int>(u % g.X());
    mark(l, x, y);
    for (const auto& s : paths[k]) {  // steps are stored goal -> start
      if (s.kind == 2) {
        ++res.vias[k];
        // The previous layer is the one whose via leads here; find it from the path's next marks: mark the tile on
        // every layer (a via column) — corridors only guide, so this is safe.
        for (int ll = 0; ll < layers; ++ll) mark(ll, x, y);
        continue;
      }
      // Horizontal edge index -> its two tiles; we are at one of them.
      int x0, y0, x1, y1, le;
      if (s.kind == 0) {
        le = static_cast<int>(s.edge / (g.Y() * (g.X() - 1)));
        const std::size_t r = s.edge % (g.Y() * (g.X() - 1));
        y0 = y1 = static_cast<int>(r / (g.X() - 1));
        x0 = static_cast<int>(r % (g.X() - 1));
        x1 = x0 + 1;
      } else {
        le = static_cast<int>(s.edge / ((g.Y() - 1) * g.X()));
        const std::size_t r = s.edge % ((g.Y() - 1) * g.X());
        y0 = static_cast<int>(r / g.X());
        x0 = x1 = static_cast<int>(r % g.X());
        y1 = y0 + 1;
      }
      l = le;
      if (x == x0 && y == y0) { x = x1; y = y1; }
      else { x = x0; y = y0; }
      mark(l, x0, y0);
      mark(l, x1, y1);
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
  return res;
}

}  // namespace tmk::route
