// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/bundle.hpp"

#include "route/escape.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <queue>
#include <string>
#include <tuple>

namespace tmk::route {

namespace {

using geom::Point;

std::size_t z(int i) { return static_cast<std::size_t>(i); }

constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr double kInvSqrt2 = 0.70710678118654752440;

struct Vec {
  double x = 0, y = 0;
};
Vec unit(int d) { return (d & 1) ? Vec{kDx[d] * kInvSqrt2, kDy[d] * kInvSqrt2} : Vec{static_cast<double>(kDx[d]), static_cast<double>(kDy[d])}; }
Vec left_normal(Vec u) { return {-u.y, u.x}; }
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
Point rounded(double x, double y) { return {static_cast<Coord>(std::llround(x)), static_cast<Coord>(std::llround(y))}; }
double mm(Coord v) { return static_cast<double>(v) / 1e6; }
std::string pad_name(const model::Board& b, int pad) {
  const auto& p = b.pads[static_cast<std::size_t>(pad)];
  return (p.footprint >= 0 ? b.footprints[static_cast<std::size_t>(p.footprint)].reference : std::string("?")) + "." + p.number;
}
double dist(Point a, Point b) { return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y)); }

int lowest_layer(model::LayerMask m) {
  for (int l = 0; l < 64; ++l)
    if (m & (model::LayerMask{1} << l)) return l;
  return -1;
}

struct Lane {
  BundleConn c;
  int src = -1, dst = -1;  // board pads
  Coord lat = 0;           // across the leaving direction (n0), pad centre
  Coord w = 0;
};

struct Group {
  int src_fp = -1, src_dir = 0, dst_fp = -1, dst_dir = 0;  // dst_dir: outward direction of the destination side that the lanes arrive at
  std::vector<Lane> lanes;
};

// The sides of a footprint's pad-centre box, as the escape plan sees a package: which side of the box a pad is nearest to.
struct FpBox {
  Coord x0 = LLONG_MAX, y0 = LLONG_MAX, x1 = LLONG_MIN, y1 = LLONG_MIN;
};

}  // namespace

std::vector<int> perimeter_directions(const model::Board& b, const BundleOptions& o) {
  std::vector<int> out(b.pads.size(), -1);
  for (const auto& fp : b.footprints) {
    std::vector<int> pads;
    for (int pi : fp.pads) {
      const auto& p = b.pads[z(pi)];
      if (p.copper != 0 && p.type != model::PadType::NpThruHole) pads.push_back(pi);
    }
    if (static_cast<int>(pads.size()) < o.min_pads) continue;
    Coord pitch = LLONG_MAX;
    for (std::size_t i = 0; i < pads.size(); ++i)
      for (std::size_t j = i + 1; j < pads.size(); ++j) {
        const Point d = b.pads[z(pads[j])].pos - b.pads[z(pads[i])].pos;
        const Coord dd = static_cast<Coord>(std::llround(std::hypot(static_cast<double>(d.x), static_cast<double>(d.y))));
        if (dd > 0) pitch = std::min(pitch, dd);
      }
    if (pitch == LLONG_MAX || pitch > o.max_pitch) continue;
    FpBox bx;
    for (int pi : pads) {
      const Point q = b.pads[z(pi)].pos;
      bx.x0 = std::min(bx.x0, q.x), bx.x1 = std::max(bx.x1, q.x), bx.y0 = std::min(bx.y0, q.y), bx.y1 = std::max(bx.y1, q.y);
    }
    const Coord ring = pitch * 3 / 4;
    for (int pi : pads) {
      const auto& p = b.pads[z(pi)];
      if (p.type != model::PadType::Smd) continue;
      const Coord dl = p.pos.x - bx.x0, dr = bx.x1 - p.pos.x, dt = p.pos.y - bx.y0, db = bx.y1 - p.pos.y;
      const int near = (dl <= ring) + (dr <= ring) + (dt <= ring) + (db <= ring);
      if (near != 1) continue;  // inside, or a corner pin
      out[z(pi)] = dl <= ring ? 4 : dr <= ring ? 0 : dt <= ring ? 6 : 2;
    }
  }
  return out;
}

namespace {

// The spine: an octilinear polyline from `s0` (leaving in direction `d0`) to the neighbourhood of `goal`, for a track `width` wide. A* on a grid of step
// `g`; a turn is 45 degrees and only after `run` steps since the last, so that the inner lanes of a turn keep a positive length. Edge legality is the
// exact check of a track as wide as the whole block (conservative: a block wider than the free channel is not placed).
struct Spine {
  std::vector<Point> corner;  // s0, the corners, the last point
  std::vector<int> dir;       // direction index of the segment after each corner (corner.size() - 1 entries)
};

bool find_spine(Point s0, int d0, Point goal, Coord g, int run, Coord width, int layer, model::NetId net, const BundleHooks& h, Spine& out) {
  const Coord margin = 12'000'000;
  const Coord bx0 = std::min(s0.x, goal.x) - margin, bx1 = std::max(s0.x, goal.x) + margin;
  const Coord by0 = std::min(s0.y, goal.y) - margin, by1 = std::max(s0.y, goal.y) + margin;
  const int i0 = static_cast<int>((s0.x - bx0) / g) + 1, j0 = static_cast<int>((s0.y - by0) / g) + 1;  // grid index of s0
  const int nx = static_cast<int>((bx1 - bx0) / g) + 3, ny = static_cast<int>((by1 - by0) / g) + 3;
  const std::int64_t nstate = static_cast<std::int64_t>(nx) * ny * 8 * (run + 1);
  if (nstate > 60'000'000) return false;
  auto node_pt = [&](int i, int j) { return Point{s0.x + static_cast<Coord>(i - i0) * g, s0.y + static_cast<Coord>(j - j0) * g}; };
  auto sid = [&](int i, int j, int d, int r) { return ((static_cast<std::int64_t>(j) * nx + i) * 8 + d) * (run + 1) + r; };
  std::vector<std::int64_t> best(static_cast<std::size_t>(nstate), LLONG_MAX);
  std::vector<std::int64_t> parent(static_cast<std::size_t>(nstate), -1);
  std::vector<std::uint8_t> edge(static_cast<std::size_t>(static_cast<std::int64_t>(nx) * ny * 8), 0);  // 0 unknown, 1 free, 2 blocked
  auto edge_free = [&](int i, int j, int d) {
    auto& e = edge[static_cast<std::size_t>((static_cast<std::int64_t>(j) * nx + i) * 8 + d)];
    if (e == 0) {
      const int ni = i + kDx[d], nj = j + kDy[d];
      e = (ni >= 1 && nj >= 1 && ni < nx - 1 && nj < ny - 1 && h.free(node_pt(i, j), node_pt(ni, nj), layer, width, net)) ? 1 : 2;
    }
    return e == 1;
  };
  const double gd = static_cast<double>(g);
  auto heur = [&](int i, int j) {  // octile distance to the goal
    const double dx = std::fabs(static_cast<double>(node_pt(i, j).x - goal.x)), dy = std::fabs(static_cast<double>(node_pt(i, j).y - goal.y));
    return static_cast<std::int64_t>(std::max(dx, dy) + (std::sqrt(2.0) - 1.0) * std::min(dx, dy));
  };
  using QE = std::tuple<std::int64_t, std::int64_t, std::int64_t>;  // f, seq, state
  std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
  std::int64_t seq = 0;
  const std::int64_t s_start = sid(i0, j0, d0, run);
  best[static_cast<std::size_t>(s_start)] = 0;
  pq.push({heur(i0, j0), seq++, s_start});
  const std::int64_t turn_pen = static_cast<std::int64_t>(6 * gd);
  long expanded = 0;
  std::int64_t goal_state = -1;
  double closest = 1e300;
  Point closest_pt = s0;
  while (!pq.empty() && expanded < 3'000'000) {
    const auto [f, sq, st] = pq.top();
    pq.pop();
    (void)sq;
    const std::int64_t cur = best[static_cast<std::size_t>(st)];
    int r = static_cast<int>(st % (run + 1));
    std::int64_t rest = st / (run + 1);
    const int d = static_cast<int>(rest % 8);
    rest /= 8;
    const int i = static_cast<int>(rest % nx), j = static_cast<int>(rest / nx);
    if (f - heur(i, j) > cur) continue;  // stale entry
    ++expanded;
    if (dist(node_pt(i, j), goal) < closest) closest = dist(node_pt(i, j), goal), closest_pt = node_pt(i, j);
    if (dist(node_pt(i, j), goal) <= 1.5 * gd) {
      goal_state = st;
      break;
    }
    for (int turn : {0, 1, -1}) {
      if (turn != 0 && r < run) continue;
      const int nd = (d + turn + 8) % 8;
      if (!edge_free(i, j, nd)) continue;
      const int ni = i + kDx[nd], nj = j + kDy[nd];
      const int nr = turn != 0 ? 1 : std::min(run, r + 1);
      const std::int64_t len = static_cast<std::int64_t>((nd & 1) ? gd * std::sqrt(2.0) : gd);
      const std::int64_t ng = cur + len + (turn != 0 ? turn_pen : 0);
      const std::int64_t ns = sid(ni, nj, nd, nr);
      if (ng >= best[static_cast<std::size_t>(ns)]) continue;
      best[static_cast<std::size_t>(ns)] = ng;
      parent[static_cast<std::size_t>(ns)] = st;
      pq.push({ng + heur(ni, nj), seq++, ns});
    }
  }
  if (goal_state < 0) {
    if (std::getenv("TM_DEBUG_BUNDLE"))
      std::fprintf(stderr, "  spine search failed: %ld expanded, closest %.2f mm at (%.2f,%.2f), start (%.2f,%.2f), goal (%.2f,%.2f), width %.2f\n", expanded, closest / 1e6, mm(closest_pt.x), mm(closest_pt.y), mm(s0.x), mm(s0.y), mm(goal.x), mm(goal.y), mm(width));
    return false;
  }
  // Back to the start, then keep the corners.
  std::vector<std::pair<Point, int>> chain;  // (node, direction of the move that arrived)
  for (std::int64_t st = goal_state; st >= 0; st = parent[static_cast<std::size_t>(st)]) {
    std::int64_t rest = st / (run + 1);
    const int d = static_cast<int>(rest % 8);
    rest /= 8;
    chain.push_back({node_pt(static_cast<int>(rest % nx), static_cast<int>(rest / nx)), d});
  }
  std::reverse(chain.begin(), chain.end());
  out.corner.assign(1, chain.front().first);
  out.dir.clear();
  for (std::size_t k = 1; k < chain.size(); ++k) {
    if (out.dir.empty() || out.dir.back() != chain[k].second) {
      if (!out.dir.empty()) out.corner.push_back(chain[k - 1].first);
      out.dir.push_back(chain[k].second);
    }
  }
  out.corner.push_back(chain.back().first);
  return !out.dir.empty();
}

// A lane: the spine offset by `o` to the left of its direction of travel, with a mitre at every corner.
std::vector<Point> lane_base(const Spine& sp, int d0, double o, Coord extend) {
  std::vector<Point> v;
  const std::size_t m = sp.dir.size();
  for (std::size_t k = 0; k <= m; ++k) {
    const Vec np = left_normal(unit(k == 0 ? d0 : sp.dir[k - 1]));
    const Vec nn = k < m ? left_normal(unit(sp.dir[k])) : np;
    const double s = 1.0 + dot(np, nn);
    const Vec mt{(np.x + nn.x) / s, (np.y + nn.y) / s};
    v.push_back(rounded(static_cast<double>(sp.corner[k].x) + o * mt.x, static_cast<double>(sp.corner[k].y) + o * mt.y));
  }
  const Vec u = unit(sp.dir.back());
  v.push_back(rounded(static_cast<double>(v.back().x) + u.x * static_cast<double>(extend), static_cast<double>(v.back().y) + u.y * static_cast<double>(extend)));
  return v;
}

// Merges collinear neighbours and repeated points.
std::vector<Point> simplified(const std::vector<Point>& in) {
  std::vector<Point> out;
  for (const Point p : in) {
    if (!out.empty() && out.back() == p) continue;
    while (out.size() >= 2) {
      const Point a = out[out.size() - 2], c = out.back();
      // The middle point goes when it lies on the line a-p to within the rounding of the offset points (0.2 um) and the run goes on the same way.
      const double cr = static_cast<double>(c.x - a.x) * static_cast<double>(p.y - a.y) - static_cast<double>(c.y - a.y) * static_cast<double>(p.x - a.x);
      const double dt = static_cast<double>(c.x - a.x) * static_cast<double>(p.x - c.x) + static_cast<double>(c.y - a.y) * static_cast<double>(p.y - c.y);
      const double chord = dist(a, p);
      if (chord > 0 && std::fabs(cr) / chord <= 200.0 && dt > 0) out.pop_back();
      else break;
    }
    out.push_back(p);
  }
  return out;
}

}  // namespace

BundleStats route_bundles(const model::Board& b, const std::vector<BundleConn>& conns, const BundleHooks& h, const BundleOptions& o) {
  BundleStats stats;
  const bool dbg = std::getenv("TM_DEBUG_BUNDLE") != nullptr;
  const std::vector<int> perim = perimeter_directions(b, o);
  // Pad-centre boxes of every footprint (destination sides).
  std::map<int, FpBox> fpbox;
  for (const auto& fp : b.footprints) {
    FpBox bx;
    for (int pi : fp.pads) {
      const auto& p = b.pads[z(pi)];
      if (p.copper == 0 || p.type == model::PadType::NpThruHole) continue;
      bx.x0 = std::min(bx.x0, p.pos.x), bx.x1 = std::max(bx.x1, p.pos.x), bx.y0 = std::min(bx.y0, p.pos.y), bx.y1 = std::max(bx.y1, p.pos.y);
    }
    if (bx.x0 != LLONG_MAX) fpbox[static_cast<int>(&fp - b.footprints.data())] = bx;
  }
  // Groups: connections from one side of a dense package to one side of another part.
  std::map<std::tuple<int, int, int, int>, Group> groups;
  for (const auto& c : conns) {
    if (c.pad_a < 0 || c.pad_b < 0) continue;
    int src = c.pad_a, dst = c.pad_b;
    if (perim[z(src)] < 0) std::swap(src, dst);
    if (perim[z(src)] < 0) continue;
    const auto& ps = b.pads[z(src)];
    const auto& pd = b.pads[z(dst)];
    if (ps.footprint < 0 || pd.footprint < 0 || ps.footprint == pd.footprint || ps.type != model::PadType::Smd) continue;
    const int layer = lowest_layer(ps.copper);
    if (layer < 0 || !(pd.copper & model::layer_bit(layer)) || !h.layer_ok(layer, c.net)) continue;
    const auto it = fpbox.find(pd.footprint);
    if (it == fpbox.end()) continue;
    const FpBox& bx = it->second;
    // The side of the destination that the pad is nearest to (ties, as in a one-column header: the side that faces the source).
    const Coord dd[4] = {bx.x1 - pd.pos.x, bx.y1 - pd.pos.y, pd.pos.x - bx.x0, pd.pos.y - bx.y0};  // sides 0 (+x), 2 (+y), 4 (-x), 6 (-y)
    const Coord dmin = *std::min_element(dd, dd + 4);
    int side = -1;
    double face = -1e300;
    for (int s = 0; s < 4; ++s) {
      if (dd[s] > dmin + 100'000) continue;
      const Vec nrm = unit(2 * s);
      const double f = dot(nrm, Vec{static_cast<double>(ps.pos.x - pd.pos.x), static_cast<double>(ps.pos.y - pd.pos.y)});
      if (f > face) face = f, side = 2 * s;
    }
    Group& g = groups[{ps.footprint, perim[z(src)], pd.footprint, side}];
    g.src_fp = ps.footprint, g.src_dir = perim[z(src)], g.dst_fp = pd.footprint, g.dst_dir = side;
    Lane ln;
    ln.c = c;
    ln.src = src;
    ln.dst = dst;
    ln.w = h.width(c.net);
    g.lanes.push_back(ln);
  }
  // Larger groups first: they are the ones worth a block, and what a block takes the smaller ones must go round.
  std::vector<Group*> order;
  for (auto& [key, g] : groups) order.push_back(&g);
  std::stable_sort(order.begin(), order.end(), [](const Group* a, const Group* c) { return a->lanes.size() > c->lanes.size(); });
  for (Group* gp : order) {
    Group& g = *gp;
    if (dbg)
      std::fprintf(stderr, "bundle group %s side %d -> %s side %d: %zu connections%s\n", b.footprints[z(g.src_fp)].reference.c_str(), g.src_dir,
                   b.footprints[z(g.dst_fp)].reference.c_str(), g.dst_dir, g.lanes.size(), static_cast<int>(g.lanes.size()) < o.min_lanes ? " (too few)" : "");
    if (static_cast<int>(g.lanes.size()) < o.min_lanes) continue;
    ++stats.groups;
    const int d0 = g.src_dir;
    const Vec u0 = unit(d0), n0 = left_normal(u0);
    for (auto& ln : g.lanes) ln.lat = static_cast<Coord>(std::llround(dot(n0, Vec{static_cast<double>(b.pads[z(ln.src)].pos.x), static_cast<double>(b.pads[z(ln.src)].pos.y)})));
    std::sort(g.lanes.begin(), g.lanes.end(), [](const Lane& a, const Lane& c) { return a.lat != c.lat ? a.lat < c.lat : a.c.ci < c.c.ci; });
    Coord lo = LLONG_MAX, hi = LLONG_MIN, wmax = 0, clr = 0;
    double along = -1e300;
    for (const auto& ln : g.lanes) {
      lo = std::min(lo, ln.lat), hi = std::max(hi, ln.lat), wmax = std::max(wmax, ln.w), clr = std::max(clr, h.clearance(ln.c.net));
      const auto [hx, hy] = pad_half_extents(b.pads[z(ln.src)]);
      const Point pp = b.pads[z(ln.src)].pos;
      along = std::max(along, dot(u0, Vec{static_cast<double>(pp.x), static_cast<double>(pp.y)}) + static_cast<double>((d0 & 2) ? hy : hx));
    }
    stats.lanes += static_cast<int>(g.lanes.size());
    const Coord centre = (lo + hi) / 2;
    const Coord wf = (hi - lo) + wmax;  // the block's width
    const Point s0 = rounded(u0.x * (along + static_cast<double>(clr + wf / 2 + 20'000)) + n0.x * static_cast<double>(centre),
                             u0.y * (along + static_cast<double>(clr + wf / 2 + 20'000)) + n0.y * static_cast<double>(centre));
    // The goal: in front of the destination pads, outside the side that the lanes arrive at.
    const FpBox& db = fpbox[g.dst_fp];
    double cx = 0, cy = 0, hn = 0;
    for (const auto& ln : g.lanes) {
      const auto& pd = b.pads[z(ln.dst)];
      cx += static_cast<double>(pd.pos.x), cy += static_cast<double>(pd.pos.y);
      const auto [hx, hy] = pad_half_extents(pd);
      hn = std::max(hn, static_cast<double>((g.dst_dir & 2) ? hy : hx));
    }
    cx /= static_cast<double>(g.lanes.size()), cy /= static_cast<double>(g.lanes.size());
    const double out_d = hn + static_cast<double>(clr + wf / 2 + 100'000);
    Point goal;
    switch (g.dst_dir) {
      case 0: goal = rounded(static_cast<double>(db.x1) + out_d, cy); break;
      case 2: goal = rounded(cx, static_cast<double>(db.y1) + out_d); break;
      case 4: goal = rounded(static_cast<double>(db.x0) - out_d, cy); break;
      default: goal = rounded(cx, static_cast<double>(db.y0) - out_d); break;
    }
    const int layer = lowest_layer(b.pads[z(g.lanes.front().src)].copper);
    const double minrun_mm = std::max(1.0, 2.0 * (static_cast<double>(hi - lo) / 2e6) * 0.4143 + 2.0 * static_cast<double>(o.grid) / 1e6);
    const int run = std::max(2, static_cast<int>(std::ceil(minrun_mm * 1e6 / static_cast<double>(o.grid))));
    Spine sp;
    if (!find_spine(s0, d0, goal, o.grid, run, wf, layer, g.lanes[g.lanes.size() / 2].c.net, h, sp)) {
      if (dbg) std::fprintf(stderr, "bundle %s side %d -> %s side %d: %zu lanes, no spine (width %.2f mm)\n", b.footprints[z(g.src_fp)].reference.c_str(), d0,
                            b.footprints[z(g.dst_fp)].reference.c_str(), g.dst_dir, g.lanes.size(), static_cast<double>(wf) / 1e6);
      continue;
    }
    ++stats.spines;
    if (dbg) {
      std::fprintf(stderr, "  spine of %s->%s:", b.footprints[z(g.src_fp)].reference.c_str(), b.footprints[z(g.dst_fp)].reference.c_str());
      for (const Point c : sp.corner) std::fprintf(stderr, " (%.2f,%.2f)", static_cast<double>(c.x) / 1e6, static_cast<double>(c.y) / 1e6);
      std::fprintf(stderr, "; goal (%.2f,%.2f)\n", static_cast<double>(goal.x) / 1e6, static_cast<double>(goal.y) / 1e6);
    }
    // Planarity: the lanes peel off toward their pads, so the lane nearest the pads' side must have the pad that comes first along the lanes' last direction.
    // Pins and pads are in whatever order the netlist gave them; the longest run of lanes that agree is the block, the others are left to the router.
    {
      const Vec ue = unit(sp.dir.back()), ne = left_normal(ue);
      const Point se = sp.corner.back();
      double lat_sum = 0;
      std::vector<double> key(g.lanes.size());
      for (std::size_t k = 0; k < g.lanes.size(); ++k) {
        const Point q = b.pads[z(g.lanes[k].dst)].pos;
        key[k] = dot(ue, Vec{static_cast<double>(q.x), static_cast<double>(q.y)});
        lat_sum += dot(ne, Vec{static_cast<double>(q.x - se.x), static_cast<double>(q.y - se.y)});
      }
      const double sigma = lat_sum >= 0 ? 1.0 : -1.0;  // the pads are to the left (+) or right (-) of the block, looking along its end
      const double gap = 300'000;                      // pads this close along the lanes would peel at the same place
      const std::size_t n = g.lanes.size();
      std::vector<int> len(n, 1), prev(n, -1);
      int best_end = 0;
      for (std::size_t k = 0; k < n; ++k) {
        for (std::size_t j = 0; j < k; ++j)
          if (sigma * (key[j] - key[k]) > gap && len[j] + 1 > len[k]) len[k] = len[j] + 1, prev[k] = static_cast<int>(j);
        if (len[k] > len[static_cast<std::size_t>(best_end)]) best_end = static_cast<int>(k);
      }
      std::vector<Lane> keep;
      for (int k = best_end; k >= 0; k = prev[static_cast<std::size_t>(k)]) keep.push_back(g.lanes[static_cast<std::size_t>(k)]);
      std::reverse(keep.begin(), keep.end());
      if (dbg && keep.size() != n) std::fprintf(stderr, "  planarity: %zu of %zu lanes agree\n", keep.size(), n);
      g.lanes = keep;
      if (static_cast<int>(g.lanes.size()) < o.min_lanes) continue;
    }
    int placed = 0;
    for (const auto& ln : g.lanes) {
      const double off = static_cast<double>(ln.lat - centre);
      const std::vector<Point> base = simplified(lane_base(sp, d0, off, o.extend));
      const Point P = b.pads[z(ln.src)].pos, Q = b.pads[z(ln.dst)].pos;
      // Walk the lane from the source pad in short pieces while each is legal; the places between are where it may peel off.
      std::vector<Point> prefix{P};
      std::vector<std::size_t> vert{0};  // index in `prefix` of each base vertex passed
      std::vector<Vec> tang{unit(d0)};
      Point cur = P;
      // The first piece runs from the pad centre to the first base vertex (the lane leaves straight).
      std::vector<Point> path = base;
      path.insert(path.begin(), P);
      bool stopped = false;
      for (std::size_t k = 1; k < path.size() && !stopped; ++k) {
        const Point a = path[k - 1], e = path[k];
        const double len = dist(a, e);
        if (len <= 0) continue;
        const Vec t{static_cast<double>(e.x - a.x) / len, static_cast<double>(e.y - a.y) / len};
        const int pieces = std::max(1, static_cast<int>(std::ceil(len / static_cast<double>(o.peel_step))));
        for (int s = 1; s <= pieces; ++s) {
          const Point nx = s == pieces ? e : rounded(static_cast<double>(a.x) + static_cast<double>(e.x - a.x) * s / pieces, static_cast<double>(a.y) + static_cast<double>(e.y - a.y) * s / pieces);
          if (!h.free(cur, nx, layer, ln.w, ln.c.net)) {
            stopped = true;
            break;
          }
          prefix.push_back(nx);
          tang.push_back(t);
          cur = nx;
        }
      }
      bool done = false;
      int n_behind = 0, n_turn = 0, n_free = 0;
      for (std::size_t k = prefix.size(); k-- > 1 && !done;) {
        const Point F = prefix[k];
        const Vec t = tang[k];
        const double dx = static_cast<double>(Q.x - F.x), dy = static_cast<double>(Q.y - F.y);
        if (dx * t.x + dy * t.y < 0) {  // the pad is behind
          ++n_behind;
          continue;
        }
        const Coord adx = std::llabs(Q.x - F.x), ady = std::llabs(Q.y - F.y);
        const Coord m = std::min(adx, ady);
        const Coord sx = Q.x >= F.x ? 1 : -1, sy = Q.y >= F.y ? 1 : -1;
        std::vector<std::vector<Point>> legs;
        legs.push_back({F, Point{F.x + sx * m, F.y + sy * m}, Q});          // diagonal first, then straight on
        if (adx != ady && m > 0) legs.push_back({F, Point{Q.x - sx * m, Q.y - sy * m}, Q});  // straight first, then the diagonal
        for (auto& leg : legs) {
          leg = simplified(leg);
          // No turn sharper than 45 degrees: into the leg and between its pieces.
          bool ok = leg.size() >= 2;
          Vec prev = t;
          for (std::size_t i = 1; i < leg.size() && ok; ++i) {
            const double l = dist(leg[i - 1], leg[i]);
            const Vec u{static_cast<double>(leg[i].x - leg[i - 1].x) / l, static_cast<double>(leg[i].y - leg[i - 1].y) / l};
            ok = dot(prev, u) >= 0.7;
            prev = u;
          }
          if (!ok) ++n_turn;
          for (std::size_t i = 1; i < leg.size() && ok; ++i) {
            ok = h.free(leg[i - 1], leg[i], layer, ln.w, ln.c.net);
            if (!ok && dbg && std::llabs(Q.y - F.y) < 2'000'000 && std::getenv("TM_DEBUG_BUNDLE_LEGS"))
              std::fprintf(stderr, "    leg fails: F (%.2f,%.2f) piece (%.2f,%.2f)-(%.2f,%.2f)\n", mm(F.x), mm(F.y), mm(leg[i - 1].x), mm(leg[i - 1].y), mm(leg[i].x), mm(leg[i].y));
          }
          if (!ok) {
            ++n_free;
            continue;
          }
          std::vector<Point> pts(prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(k) + 1);
          pts.insert(pts.end(), leg.begin() + 1, leg.end());
          pts = simplified(pts);
          if (h.place(ln.c, layer, pts)) ++placed;
          if (dbg) {
            std::fprintf(stderr, "  lane %s:", b.nets[z(ln.c.net)].name.c_str());
            for (const Point q : pts) std::fprintf(stderr, " (%.2f,%.2f)", mm(q.x), mm(q.y));
            std::fprintf(stderr, "\n");
          }
          done = true;  // placed, or refused by the router's own check: either way this lane is settled
          break;
        }
      }
      if (dbg && !done)
        std::fprintf(stderr, "bundle: lane %s %s -> %s: no peel (walked %zu pieces%s, ended at (%.2f,%.2f), pad (%.2f,%.2f); stations behind %d, turn %d, blocked %d)\n", b.nets[z(ln.c.net)].name.c_str(),
                     b.footprints[z(g.src_fp)].reference.c_str(), b.footprints[z(g.dst_fp)].reference.c_str(), prefix.size() - 1, stopped ? ", blocked" : "",
                     static_cast<double>(prefix.back().x) / 1e6, static_cast<double>(prefix.back().y) / 1e6, static_cast<double>(Q.x) / 1e6, static_cast<double>(Q.y) / 1e6, n_behind, n_turn, n_free);
    }
    stats.placed += placed;
    if (dbg)
      std::fprintf(stderr, "bundle %s side %d -> %s side %d: %zu lanes, width %.2f mm, spine %zu corners, %d placed\n", b.footprints[z(g.src_fp)].reference.c_str(), d0,
                   b.footprints[z(g.dst_fp)].reference.c_str(), g.dst_dir, g.lanes.size(), static_cast<double>(wf) / 1e6, sp.corner.size(), placed);
  }
  return stats;
}

TunnelStats route_tunnels(const model::Board& b, const std::vector<BundleConn>& conns, const BundleHooks& h, const BundleOptions& o) {
  TunnelStats stats;
  const bool dbg = std::getenv("TM_DEBUG_BUNDLE") != nullptr;
  const std::vector<int> perim = perimeter_directions(b, o);
  const int ncu = b.copper_count();
  for (const auto& c : conns) {
    if (c.pad_a < 0 || c.pad_b < 0) continue;
    const auto& pa = b.pads[z(c.pad_a)];
    const auto& pb = b.pads[z(c.pad_b)];
    if (perim[z(c.pad_a)] < 0 || perim[z(c.pad_b)] < 0 || pa.footprint != pb.footprint || pa.type != model::PadType::Smd || pb.type != model::PadType::Smd) continue;
    if (dist(pa.pos, pb.pos) < 2'000'000) continue;  // neighbouring pins need no tunnel
    const int l0 = lowest_layer(pa.copper);
    if (l0 < 0 || lowest_layer(pb.copper) != l0 || ncu < 2) continue;
    const int l1 = l0 == 0 ? ncu - 1 : 0;  // the other outer layer
    if (!h.layer_ok(l0, c.net) || !h.layer_ok(l1, c.net)) continue;
    ++stats.tried;
    const Coord w = h.width(c.net), clr = h.clearance(c.net), vd = h.via_diameter(c.net);
    // A via straight out of each pin: the nearest place where the stub and the via pass the exact check.
    Point via[2];
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e) {
      const int pad = e == 0 ? c.pad_a : c.pad_b;
      const auto& pd = b.pads[z(pad)];
      const int d = perim[z(pad)];
      const Vec u = unit(d);
      const auto [hx, hy] = pad_half_extents(pd);
      const double base = static_cast<double>((d & 2) ? hy : hx) + static_cast<double>(vd / 2 + clr + 20'000);
      bool found = false;
      for (double t = base; t <= base + 1'800'000.0 && !found; t += 100'000.0) {
        const Point v = rounded(static_cast<double>(pd.pos.x) + u.x * t, static_cast<double>(pd.pos.y) + u.y * t);
        if (h.free(pd.pos, v, l0, w, c.net) && h.via_ok(v, c.net)) via[e] = v, found = true;
      }
      ok = found;
    }
    if (!ok) {
      if (dbg) std::fprintf(stderr, "tunnel %s: %s -> %s: no via site\n", b.nets[z(c.net)].name.c_str(), pad_name(b, c.pad_a).c_str(), pad_name(b, c.pad_b).c_str());
      continue;
    }
    // The run on the other layer between the two via sites.
    const Vec dv{static_cast<double>(via[1].x - via[0].x), static_cast<double>(via[1].y - via[0].y)};
    int d0 = 0;
    {
      double best = -2;
      for (int k = 0; k < 8; ++k)
        if (dot(unit(k), dv) > best) best = dot(unit(k), dv), d0 = k;
    }
    Spine sp;
    const Coord g = 250'000;
    if (!find_spine(via[0], d0, via[1], g, 2, w, l1, c.net, h, sp)) {
      if (dbg) std::fprintf(stderr, "tunnel %s: %s -> %s: no run on layer %d\n", b.nets[z(c.net)].name.c_str(), pad_name(b, c.pad_a).c_str(), pad_name(b, c.pad_b).c_str(), l1);
      continue;
    }
    std::vector<Point> run = sp.corner;
    if (!(run.back() == via[1])) {
      if (!h.free(run.back(), via[1], l1, w, c.net)) continue;
      run.push_back(via[1]);
    }
    run = simplified(run);
    std::vector<std::pair<int, std::vector<Point>>> tracks;
    tracks.push_back({l0, {pa.pos, via[0]}});
    if (run.size() >= 2) tracks.push_back({l1, run});
    tracks.push_back({l0, {via[1], pb.pos}});
    if (h.place_tunnel(c, tracks, {via[0], via[1]})) ++stats.placed;
    if (dbg) std::fprintf(stderr, "tunnel %s: %s -> %s: %s (vias at (%.2f,%.2f) (%.2f,%.2f), run of %zu points)\n", b.nets[z(c.net)].name.c_str(), pad_name(b, c.pad_a).c_str(),
                          pad_name(b, c.pad_b).c_str(), "placed", mm(via[0].x), mm(via[0].y), mm(via[1].x), mm(via[1].y), run.size());
  }
  return stats;
}

}  // namespace tmk::route
