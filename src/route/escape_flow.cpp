#include "route/escape_flow.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <tuple>

namespace tmk::route {

namespace {

std::size_t z(int i) { return static_cast<std::size_t>(i); }

std::int64_t isqrt(std::int64_t v) {
  if (v <= 0) return 0;
  auto r = static_cast<std::int64_t>(std::sqrt(static_cast<double>(v)));
  while (r * r > v) --r;
  while ((r + 1) * (r + 1) <= v) ++r;
  return r;
}

int lowest_layer(model::LayerMask m) {
  for (int l = 0; l < 64; ++l)
    if (m & (model::LayerMask{1} << l)) return l;
  return -1;
}

// Min-cost max flow by successive shortest paths (Dijkstra on reduced costs; all arc costs are non-negative
// integers, so the first potentials are 0). Ties in Dijkstra are broken by node index and arcs are scanned in a
// fixed order, so the flow is deterministic.
class Flow {
 public:
  explicit Flow(int n) : head_(z(n), -1) {}
  int arc(int u, int v, int cap, std::int64_t cost) {
    arcs_.push_back({v, head_[z(u)], cap, 0, cost});
    head_[z(u)] = static_cast<int>(arcs_.size()) - 1;
    arcs_.push_back({u, head_[z(v)], 0, 0, -cost});
    head_[z(v)] = static_cast<int>(arcs_.size()) - 1;
    return static_cast<int>(arcs_.size()) - 2;
  }
  int run(int s, int t) {
    const std::size_t n = head_.size();
    constexpr std::int64_t inf = INT64_MAX / 4;
    std::vector<std::int64_t> pot(n, 0), dist(n);
    std::vector<int> prev(n);
    int total = 0;
    using QE = std::pair<std::int64_t, int>;
    for (;;) {
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
          if (a.cap - a.flow <= 0) continue;
          const std::int64_t nd = d + a.cost + pot[z(u)] - pot[z(a.to)];
          if (nd < dist[z(a.to)]) {
            dist[z(a.to)] = nd;
            prev[z(a.to)] = e;
            pq.emplace(nd, a.to);
          }
        }
      }
      if (dist[z(t)] >= inf) break;
      for (std::size_t v = 0; v < n; ++v)
        if (dist[v] < inf) pot[v] += dist[v];
      int push = INT32_MAX;
      for (int v = t; v != s; v = arcs_[z(prev[z(v)] ^ 1)].to) push = std::min(push, arcs_[z(prev[z(v)])].cap - arcs_[z(prev[z(v)])].flow);
      for (int v = t; v != s; v = arcs_[z(prev[z(v)] ^ 1)].to) {
        arcs_[z(prev[z(v)])].flow += push;
        arcs_[z(prev[z(v)] ^ 1)].flow -= push;
      }
      total += push;
    }
    return total;
  }
  // Takes one unit of flow off the first forward arc out of u that carries flow; returns the arc or -1.
  int take(int u) {
    for (int e = head_[z(u)]; e >= 0; e = arcs_[z(e)].next)
      if ((e & 1) == 0 && arcs_[z(e)].flow > 0) {
        --arcs_[z(e)].flow;
        return e;
      }
    return -1;
  }
  int to(int e) const { return arcs_[z(e)].to; }
  int flow(int e) const { return arcs_[z(e)].flow; }

 private:
  struct Arc {
    int to, next, cap, flow;
    std::int64_t cost;
  };
  std::vector<int> head_;
  std::vector<Arc> arcs_;
};

// A deep ball-grid array.
struct Grid {
  int fp = -1;
  Coord p = 0, x0 = 0, y0 = 0;
  int nx = 0, ny = 0, layer = -1;
  std::vector<int> ball;  // nx * ny: board pad index or -1
  int at(int i, int j) const { return i < 0 || j < 0 || i >= nx || j >= ny ? -1 : ball[z(j * nx + i)]; }
  Point pos(int i, int j) const { return {x0 + static_cast<Coord>(i) * p, y0 + static_cast<Coord>(j) * p}; }
  int ring(int i, int j) const { return std::min({i, j, nx - 1 - i, ny - 1 - j}) + 1; }
};

bool detect(const model::Board& b, const model::Footprint& fp, const std::vector<char>& needs, const EscapeOptions& o, Grid& g) {
  std::vector<int> pads;
  for (int pi : fp.pads) {
    const auto& p = b.pads[z(pi)];
    if (p.copper != 0 && p.type != model::PadType::NpThruHole) pads.push_back(pi);
  }
  if (static_cast<int>(pads.size()) < o.min_pads) return false;
  Coord pitch = LLONG_MAX;  // as version 1
  for (std::size_t i = 0; i < pads.size(); ++i)
    for (std::size_t j = i + 1; j < pads.size(); ++j) {
      const Point d = b.pads[z(pads[j])].pos - b.pads[z(pads[i])].pos;
      const Coord dist = static_cast<Coord>(std::llround(std::hypot(static_cast<double>(d.x), static_cast<double>(d.y))));
      if (dist > 0) pitch = std::min(pitch, dist);
    }
  if (pitch == LLONG_MAX || pitch > o.max_pitch) return false;
  std::vector<int> balls;
  int layer = -1;
  for (int pi : pads) {
    const auto& p = b.pads[z(pi)];
    if (p.type != model::PadType::Smd) return false;
    if (std::max(p.size_x, p.size_y) >= pitch) continue;  // a thermal pad: left to the fixed copper checks
    const int l = lowest_layer(p.copper);
    if (layer >= 0 && l != layer) return false;
    layer = l;
    balls.push_back(pi);
  }
  if (balls.size() < 25) return false;
  // Grid origin per axis: the offset (modulo the pitch) most balls share. Balls off that grid (test or mounting
  // pads, a second sub-array at another offset) are left to the fixed copper checks, if at most 20 % are off it.
  const Coord tol = pitch / 8;
  auto fdiv = [](Coord a, Coord d) { return a >= 0 ? a / d : -((-a + d - 1) / d); };
  auto origin = [&](bool xa) {
    Coord base = LLONG_MAX;
    for (int pi : balls) base = std::min(base, xa ? b.pads[z(pi)].pos.x : b.pads[z(pi)].pos.y);
    Coord best_r = 0;
    int best_n = -1;
    for (int pi : balls) {
      const Coord r = ((xa ? b.pads[z(pi)].pos.x : b.pads[z(pi)].pos.y) - base) % pitch;
      int n = 0;
      for (int pj : balls) {
        const Coord rj = ((xa ? b.pads[z(pj)].pos.x : b.pads[z(pj)].pos.y) - base) % pitch;
        const Coord d = std::llabs(rj - r);
        n += std::min(d, pitch - d) <= tol;
      }
      if (n > best_n || (n == best_n && r < best_r)) best_n = n, best_r = r;
    }
    return base + best_r;
  };
  const Coord ox = origin(true), oy = origin(false);
  auto snap = [&](Coord v, Coord o0, Coord& k) {
    k = fdiv(v - o0 + pitch / 2, pitch);
    return std::llabs(v - o0 - k * pitch) <= tol;
  };
  std::vector<std::tuple<Coord, Coord, int>> on;
  Coord kx0 = LLONG_MAX, ky0 = LLONG_MAX, kx1 = LLONG_MIN, ky1 = LLONG_MIN;
  for (int pi : balls) {
    Coord kx = 0, ky = 0;
    if (!snap(b.pads[z(pi)].pos.x, ox, kx) || !snap(b.pads[z(pi)].pos.y, oy, ky)) continue;
    on.emplace_back(kx, ky, pi);
    kx0 = std::min(kx0, kx), kx1 = std::max(kx1, kx), ky0 = std::min(ky0, ky), ky1 = std::max(ky1, ky);
  }
  if (on.size() < 25 || on.size() * 5 < balls.size() * 4) return false;
  g = Grid{};
  g.p = pitch, g.layer = layer;
  g.x0 = ox + kx0 * pitch, g.y0 = oy + ky0 * pitch;
  if (kx1 - kx0 + 1 > 100 || ky1 - ky0 + 1 > 100) return false;
  g.nx = static_cast<int>(kx1 - kx0 + 1), g.ny = static_cast<int>(ky1 - ky0 + 1);
  if (g.nx < 5 || g.ny < 5) return false;
  if (static_cast<long>(on.size()) * 10 < 3L * g.nx * g.ny) return false;
  g.ball.assign(z(g.nx * g.ny), -1);
  bool deep = false;
  for (const auto& [kx, ky, pi] : on) {
    const int i = static_cast<int>(kx - kx0), j = static_cast<int>(ky - ky0);
    auto& cell = g.ball[z(j * g.nx + i)];
    if (cell >= 0) return false;
    cell = pi;
    if (needs[z(pi)] && g.ring(i, j) >= 3) deep = true;
  }
  return deep;
}

// One layer's escape graph. Obstacles (balls on the pad layer, via sites on the others) sit on a "corner" grid of
// cx x cy points at `origin` + (i, j) * p; the graph nodes are the (cx + 1) x (cy + 1) square gaps between them,
// gap (ei, ej) centred at origin + (ei - 1/2, ej - 1/2) * p. Gaps with corners on every side are inside the array;
// the others are outside (reaching one is escaping). Sources sit on corners.
struct Corner {
  Coord rx = 0, ry = 0, rd = 0;  // radius along x, along y and along the diagonals (0: no obstacle)
};
struct LayerModel {
  int cx = 0, cy = 0, layer = -1;
  Point origin;
  Coord p = 0;
  std::vector<Corner> corners;
  const Corner& corner(int i, int j) const {
    static const Corner none{};
    return i < 0 || j < 0 || i >= cx || j >= cy ? none : corners[z(j * cx + i)];
  }
  int ex() const { return cx + 1; }
  int ey() const { return cy + 1; }
  bool inside(int ei, int ej) const { return ei >= 1 && ej >= 1 && ei <= cx - 1 && ej <= cy - 1; }
  Point centre(int ei, int ej) const {
    return {origin.x + static_cast<Coord>(ei) * p - p / 2, origin.y + static_cast<Coord>(ej) * p - p / 2};
  }
};

struct Rules {
  Coord tw = 0, s = 0, dv = 0;
  model::NetId rep = 0;  // a net with these rules, for the fixed copper checks
  int tracks(Coord gap) const { return gap >= tw + 2 * s ? static_cast<int>(1 + (gap - tw - 2 * s) / (tw + s)) : 0; }
};

struct Source {
  int ci = 0, cj = 0;   // corner
  model::NetId net = 0;
};
struct LayerResult {
  std::vector<std::vector<Point>> path;  // per source: element centres and channel points out of the array (empty: none)
  std::vector<char> direct;              // per source: straight into an outside gap (no channel)
  std::vector<char> crowded;             // per source: shares a channel with another escape
  std::vector<int> used;                 // per gap: escapes through it (or starting in it)
};

// Cost units: tenths of a pitch (half diagonal 7, one step 10).
constexpr std::int64_t kHalfDiag = 7, kStep = 10, kDirect = 5;

LayerResult solve_layer(const LayerModel& m, const std::vector<Source>& src, const Rules& r, const FlowEscapeInput& in, Coord ext) {
  const int ex = m.ex(), ey = m.ey(), ne = ex * ey, ns = static_cast<int>(src.size());
  const int S = 0, T = 1;
  auto ein = [&](int ei, int ej) { return 2 + ns + 2 * (ej * ex + ei); };
  Flow f(2 + ns + 2 * ne);
  const Coord diag = isqrt(2 * m.p * m.p);
  // Node capacities of the gaps inside the array.
  for (int ej = 1; ej < ey - 1; ++ej)
    for (int ei = 1; ei < ex - 1; ++ei) {
      const Corner &a = m.corner(ei - 1, ej - 1), &b = m.corner(ei, ej), &c = m.corner(ei, ej - 1), &d = m.corner(ei - 1, ej);
      int cap = std::min(r.tracks(diag - a.rd - b.rd), r.tracks(diag - c.rd - d.rd));
      if (cap > 0 && in.track_free && !in.track_free(m.layer, m.centre(ei, ej), r.rep)) cap = 0;
      f.arc(ein(ei, ej), ein(ei, ej) + 1, cap, 0);
    }
  // Channels between neighbouring gaps (at least one inside). Arc metadata: channel id, direction and the
  // channel's midpoint, by arc index.
  struct Ch {
    int id;
    bool horizontal;  // the step is along x (the channel is between two corners one above the other)
    Point mid;        // midpoint of the two corners
    Coord shift;      // centre of the free gap relative to mid, across the step (corners of unequal size)
    int dir;          // +1 or -1 along the step axis
    bool exit;
  };
  std::map<int, Ch> meta;
  auto channel = [&](int ei, int ej, bool horizontal) {  // between (ei, ej) and its +x (or +y) neighbour
    Coord gap;
    Point mid;
    Coord shift;
    if (horizontal) {
      const Corner &a = m.corner(ei, ej - 1), &b = m.corner(ei, ej);
      gap = m.p - a.ry - b.ry;
      shift = (a.ry - b.ry) / 2;
      mid = {m.origin.x + static_cast<Coord>(ei) * m.p, m.origin.y + static_cast<Coord>(ej) * m.p - m.p / 2};
    } else {
      const Corner &a = m.corner(ei - 1, ej), &b = m.corner(ei, ej);
      gap = m.p - a.rx - b.rx;
      shift = (a.rx - b.rx) / 2;
      mid = {m.origin.x + static_cast<Coord>(ei) * m.p - m.p / 2, m.origin.y + static_cast<Coord>(ej) * m.p};
    }
    int cap = r.tracks(gap);
    if (cap > 0 && in.track_free) {
      const Point gc = horizontal ? Point{mid.x, mid.y + shift} : Point{mid.x + shift, mid.y};
      if (!in.track_free(m.layer, gc, r.rep)) cap = 0;
    }
    const int ni = horizontal ? ei + 1 : ei, nj = horizontal ? ej : ej + 1;
    const bool ia = m.inside(ei, ej), ib = m.inside(ni, nj);
    if (cap <= 0 || (!ia && !ib)) return;
    const int id = (ej * ex + ei) * 2 + (horizontal ? 0 : 1);
    if (ia) meta[f.arc(ein(ei, ej) + 1, ib ? ein(ni, nj) : T, cap, kStep)] = {id, horizontal, mid, shift, +1, !ib};
    if (ib) meta[f.arc(ein(ni, nj) + 1, ia ? ein(ei, ej) : T, cap, kStep)] = {id, horizontal, mid, shift, -1, !ia};
  };
  for (int ej = 0; ej < ey; ++ej)
    for (int ei = 0; ei < ex; ++ei) {
      if (ei + 1 < ex) channel(ei, ej, true);
      if (ej + 1 < ey) channel(ei, ej, false);
    }
  // Sources into the four gaps around their corner.
  std::map<int, std::pair<int, int>> start;  // arc -> gap it enters
  for (int k = 0; k < ns; ++k) {
    f.arc(S, 2 + k, 1, 0);
    for (int dj = 0; dj <= 1; ++dj)
      for (int di = 0; di <= 1; ++di) {
        const int ei = src[z(k)].ci + di, ej = src[z(k)].cj + dj;
        if (m.inside(ei, ej)) start[f.arc(2 + k, ein(ei, ej), 1, kHalfDiag)] = {ei, ej};
        else start[f.arc(2 + k, T, 1, kDirect)] = {ei, ej};
      }
  }
  f.run(S, T);
  LayerResult res;
  res.path.assign(z(ns), {});
  res.direct.assign(z(ns), 0);
  res.crowded.assign(z(ns), 0);
  res.used.assign(z(ne), 0);
  // Decompose into one path per source (sources in order, arcs in fixed order), then place each channel's
  // escapes side by side at the track pitch.
  std::vector<std::vector<int>> arcs(z(ns));
  std::map<int, int> load;
  for (int k = 0; k < ns; ++k) {
    int u = 2 + k;
    std::vector<int> walk;
    for (int guard = 0; u != T && guard < 4 * ne + 8; ++guard) {
      const int e = f.take(u);
      if (e < 0) break;
      walk.push_back(e);
      u = f.to(e);
    }
    if (u != T || walk.empty()) continue;
    arcs[z(k)] = walk;
    for (int e : walk)
      if (auto it = meta.find(e); it != meta.end()) ++load[it->second.id];
  }
  std::map<int, int> slot;
  const Coord q = r.tw + r.s;
  for (int k = 0; k < ns; ++k) {
    const auto& walk = arcs[z(k)];
    if (walk.empty()) continue;
    const auto [si, sj] = start.at(walk.front());
    auto& pts = res.path[z(k)];
    if (!m.inside(si, sj)) {
      res.direct[z(k)] = 1;
      pts.push_back(m.centre(si, sj));
      continue;
    }
    pts.push_back(m.centre(si, sj));
    ++res.used[z(sj * ex + si)];
    for (std::size_t w = 1; w < walk.size(); ++w) {
      auto it = meta.find(walk[w]);
      if (it == meta.end()) continue;  // a gap's own node arc
      const Ch& c = it->second;
      const int n = load[c.id], s = slot[c.id]++;
      if (n > 1) res.crowded[z(k)] = 1;
      const Coord off = (static_cast<Coord>(2 * s - (n - 1)) * q) / 2;  // across the step
      const Point at = c.horizontal ? Point{c.mid.x, c.mid.y + c.shift + off} : Point{c.mid.x + c.shift + off, c.mid.y};
      pts.push_back(at);
      const Point step = c.horizontal ? Point{c.dir * m.p, 0} : Point{0, c.dir * m.p};
      if (c.exit) {
        const Coord len = ext;
        pts.push_back(c.horizontal ? Point{at.x + c.dir * len, at.y} : Point{at.x, at.y + c.dir * len});
      } else {
        const Point next{c.mid.x + step.x / 2, c.mid.y + step.y / 2};  // the next gap's centre
        pts.push_back(next);
        const int ni = static_cast<int>((next.x - m.origin.x + m.p / 2) / m.p), nj = static_cast<int>((next.y - m.origin.y + m.p / 2) / m.p);
        if (ni >= 0 && nj >= 0 && ni < ex && nj < ey) ++res.used[z(nj * ex + ni)];
      }
    }
  }
  return res;
}

std::tuple<Coord, Coord, Coord> radii(const model::Pad& p) {
  const auto [hx, hy] = pad_half_extents(p);
  const Coord rd = p.shape == model::PadShape::Circle ? std::max(hx, hy) : isqrt(hx * hx + hy * hy);
  return {hx, hy, rd};
}

}  // namespace

std::vector<int> array_rings(const model::Board& b, const std::vector<char>& needs, const EscapeOptions& o) {
  std::vector<int> ring(b.pads.size(), 0);
  for (const auto& fp : b.footprints) {
    Grid g;
    if (!detect(b, fp, needs, o, g)) continue;
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i)
        if (g.at(i, j) >= 0) ring[z(g.at(i, j))] = g.ring(i, j);
  }
  return ring;
}

std::vector<EscapeCorridor> plan_escapes_flow(const model::Board& b, const std::vector<char>& needs, const FlowEscapeInput& in,
                                              const EscapeOptions& o, FlowEscapeStats* stats) {
  // Version 1 for every package that is not a deep array.
  std::vector<char> rest = needs;
  std::vector<Grid> grids;
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    Grid g;
    if (!detect(b, b.footprints[fi], needs, o, g)) continue;
    g.fp = static_cast<int>(fi);
    for (int pi : b.footprints[fi].pads) rest[z(pi)] = 0;
    grids.push_back(std::move(g));
  }
  std::vector<EscapeCorridor> out = plan_escapes(b, rest, in.keep, o);
  if (stats) {
    stats->per_layer.assign(z(std::max(in.layers, 1)), 0);
    stats->arrays = static_cast<int>(grids.size());
  }
  for (const Grid& g : grids) {
    // Planning rules: the most common (width, clearance, via) among the pins to route; ties: the smallest.
    std::map<std::tuple<Coord, Coord, Coord>, std::pair<int, model::NetId>> votes;
    for (int pi : g.ball)
      if (pi >= 0 && needs[z(pi)]) {
        const model::NetId n = b.pads[z(pi)].net;
        auto& v = votes[{in.width(n), in.clearance(n), in.via(n)}];
        if (v.first++ == 0) v.second = n;
      }
    Rules r;
    int best = -1;
    for (const auto& [k, v] : votes)
      if (v.first > best) {
        best = v.first;
        std::tie(r.tw, r.s, r.dv) = k;
        r.rep = v.second;
      }
    const Coord p = g.p;
    Coord rmax = 0;
    LayerModel top;
    top.cx = g.nx, top.cy = g.ny, top.layer = g.layer, top.origin = g.pos(0, 0), top.p = p;
    top.corners.assign(z(g.nx * g.ny), {});
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i)
        if (const int pi = g.at(i, j); pi >= 0) {
          auto [rx, ry, rd] = radii(b.pads[z(pi)]);
          top.corners[z(j * g.nx + i)] = {rx, ry, rd};
          rmax = std::max({rmax, rx, ry});
        }
    const Coord ext = rmax + o.length;
    // Sources: the balls to route, in grid order (row by row).
    std::vector<int> pins;
    std::vector<Source> src;
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i)
        if (const int pi = g.at(i, j); pi >= 0 && needs[z(pi)] && b.pads[z(pi)].net > 0) {
          pins.push_back(pi);
          src.push_back({i, j, b.pads[z(pi)].net});
        }
    const int np = static_cast<int>(pins.size());
    std::vector<int> how(z(np), 0);  // 1 pad layer, 2 other layer, 3 via only, 0 none
    std::vector<EscapeCorridor> mine(z(np));
    // Bands as version 1 (0.35 pitch through the array, half a pitch for perimeter pins). Narrower bands so
    // that perimeter corridors and channel exits never overlap (0.24 / 0.15 pitch) measured no better (doc 05 §14).
    const Coord band0 = p * 35 / 100;
    const Coord bandd = p / 2 - 1;
    auto corridor = [&](int k, bool crowded) {
      EscapeCorridor& c = mine[z(k)];
      c.pad = pins[z(k)];
      c.net = src[z(k)].net;
      c.layer = g.layer;
      c.a = b.pads[z(c.pad)].pos;
      c.band = std::min(in.keep(c.net), band0);
      if (crowded) c.band = std::min(c.band, std::max<Coord>((r.tw + r.s) / 2, 1));
    };
    // 1. The pad layer.
    const LayerResult lr = solve_layer(top, src, r, in, ext);
    for (int k = 0; k < np; ++k) {
      const auto& pts = lr.path[z(k)];
      if (pts.empty()) continue;
      how[z(k)] = 1;
      corridor(k, lr.crowded[z(k)] != 0);
      EscapeCorridor& c = mine[z(k)];
      if (lr.direct[z(k)]) {  // perimeter ball straight out through the nearest side, as version 1
        const auto& pad = b.pads[z(c.pad)];
        const auto [hx, hy] = pad_half_extents(pad);
        const auto [i, j] = std::pair{src[z(k)].ci, src[z(k)].cj};
        const int dl = i, dr = g.nx - 1 - i, dt = j, db = g.ny - 1 - j, mn = std::min({dl, dr, dt, db});
        if (mn == dl) c.b = {c.a.x - hx - o.length, c.a.y};
        else if (mn == dr) c.b = {c.a.x + hx + o.length, c.a.y};
        else if (mn == dt) c.b = {c.a.x, c.a.y - hy - o.length};
        else c.b = {c.a.x, c.a.y + hy + o.length};
        c.band = std::min(in.keep(c.net), bandd);
        continue;
      }
      c.b = pts.front();
      c.tail.assign(pts.begin() + 1, pts.end());
      c.tail_layer = g.layer;
    }
    // 2. Dog-bone via sites for the others: each ball to one of the four interstitial sites around it that no
    //    pad-layer escape uses, at most one via per site, preferring the site pointing away from the centre.
    const Coord half_diag = isqrt(2 * p * p) / 2;
    std::vector<char> has_via(z((g.nx - 1) * (g.ny - 1)), 0);  // site (a, b) between balls a..a+1, b..b+1
    std::vector<int> site_of(z(np), -1);
    if (in.layers > 1 && p >= r.dv + r.s) {
      const int ns = (g.nx - 1) * (g.ny - 1);
      Flow f(2 + np + ns);
      std::map<int, std::pair<int, int>> arc_of;  // arc -> (pin, site)
      const Point centre{(g.pos(0, 0).x + g.pos(g.nx - 1, 0).x) / 2, (g.pos(0, 0).y + g.pos(0, g.ny - 1).y) / 2};
      std::vector<char> site_ok(z(ns), 0);
      for (int sb = 0; sb < g.ny - 1; ++sb)
        for (int sa = 0; sa < g.nx - 1; ++sa) {
          if (lr.used[z((sb + 1) * top.ex() + sa + 1)] > 0) continue;  // a pad-layer escape crosses it
          bool fits = true;
          for (int d = 0; d < 4 && fits; ++d) {
            const Corner& cn = top.corner(sa + (d & 1), sb + (d >> 1));
            if (half_diag < cn.rd + r.dv / 2 + r.s) fits = false;
          }
          if (!fits) continue;
          site_ok[z(sb * (g.nx - 1) + sa)] = 1;
          f.arc(2 + np + sb * (g.nx - 1) + sa, 1, 1, 0);
        }
      for (int k = 0; k < np; ++k) {
        if (how[z(k)]) continue;
        f.arc(0, 2 + k, 1, 0);
        const auto [i, j] = std::pair{src[z(k)].ci, src[z(k)].cj};
        const Point a = g.pos(i, j);
        const int sx = a.x >= centre.x ? 1 : -1, sy = a.y >= centre.y ? 1 : -1;
        for (int dj = -1; dj <= 0; ++dj)
          for (int di = -1; di <= 0; ++di) {
            const int sa = i + di, sb = j + dj;
            if (sa < 0 || sb < 0 || sa >= g.nx - 1 || sb >= g.ny - 1 || !site_ok[z(sb * (g.nx - 1) + sa)]) continue;
            const Point site{a.x + (di == 0 ? 1 : -1) * p / 2, a.y + (dj == 0 ? 1 : -1) * p / 2};
            if (in.via_free && !in.via_free(site, src[z(k)].net)) continue;
            const int away = ((di == 0 ? 1 : -1) == sx) + ((dj == 0 ? 1 : -1) == sy);
            arc_of[f.arc(2 + k, 2 + np + sb * (g.nx - 1) + sa, 1, 2 - away)] = {k, sb * (g.nx - 1) + sa};
          }
      }
      f.run(0, 1);
      for (const auto& [e, ks] : arc_of)
        if (f.flow(e) > 0) {
          site_of[z(ks.first)] = ks.second;
          has_via[z(ks.second)] = 1;
        }
    }
    // 3. Every other layer, nearest to the pad layer first: the vias are that layer's obstacles.
    std::vector<int> order;
    for (int l = 0; l < in.layers; ++l)
      if (l != g.layer) order.push_back(l);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return std::abs(x - g.layer) < std::abs(y - g.layer); });
    LayerModel inner;
    inner.cx = g.nx + 1, inner.cy = g.ny + 1, inner.p = p;
    inner.origin = {g.x0 - p / 2, g.y0 - p / 2};  // corner (ci, cj) = site (ci - 1, cj - 1)
    inner.corners.assign(z(inner.cx * inner.cy), {});
    for (int sb = 0; sb < g.ny - 1; ++sb)
      for (int sa = 0; sa < g.nx - 1; ++sa)
        if (has_via[z(sb * (g.nx - 1) + sa)]) inner.corners[z((sb + 1) * inner.cx + sa + 1)] = {r.dv / 2, r.dv / 2, r.dv / 2};
    std::vector<char> done(z(np), 0);
    for (int l : order) {
      std::vector<Source> vs;
      std::vector<int> back;
      for (int k = 0; k < np; ++k)
        if (site_of[z(k)] >= 0 && !done[z(k)]) {
          const int sa = site_of[z(k)] % (g.nx - 1), sb = site_of[z(k)] / (g.nx - 1);
          vs.push_back({sa + 1, sb + 1, src[z(k)].net});
          back.push_back(k);
        }
      if (vs.empty()) break;
      inner.layer = l;
      const LayerResult il = solve_layer(inner, vs, r, in, ext);
      for (std::size_t v = 0; v < vs.size(); ++v) {
        if (il.path[v].empty()) continue;
        const int k = back[v];
        done[z(k)] = 1;
        how[z(k)] = 2;
        corridor(k, il.crowded[v] != 0);
        EscapeCorridor& c = mine[z(k)];
        c.b = {inner.origin.x + static_cast<Coord>(vs[v].ci) * p, inner.origin.y + static_cast<Coord>(vs[v].cj) * p};
        c.via = true;
        c.tail = il.path[v];
        c.tail_layer = l;
        if (stats) ++stats->per_layer[z(l)];
      }
    }
    for (int k = 0; k < np; ++k) {
      if (how[z(k)] == 0 && site_of[z(k)] >= 0) {  // a dog-bone without a planned way out
        how[z(k)] = 3;
        corridor(k, false);
        EscapeCorridor& c = mine[z(k)];
        const int sa = site_of[z(k)] % (g.nx - 1), sb = site_of[z(k)] / (g.nx - 1);
        c.b = {inner.origin.x + static_cast<Coord>(sa + 1) * p, inner.origin.y + static_cast<Coord>(sb + 1) * p};
        c.via = true;
      }
      if (how[z(k)] == 1 && stats) ++stats->per_layer[z(g.layer)];
      if (stats) {
        const int rg = g.ring(src[z(k)].ci, src[z(k)].cj);
        if (static_cast<int>(stats->rings.size()) < rg) stats->rings.resize(z(rg));
        auto& rp = stats->rings[z(rg - 1)];
        ++rp.pins;
        if (how[z(k)] == 1) ++rp.pad_layer;
        else if (how[z(k)] == 2) ++rp.other_layer;
        else if (how[z(k)] == 3) ++rp.via_only;
        else ++rp.none;
      }
      if (how[z(k)] != 0 && mine[z(k)].band > 0) out.push_back(mine[z(k)]);
    }
  }
  return out;
}

}  // namespace tmk::route
