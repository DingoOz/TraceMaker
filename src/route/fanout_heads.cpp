// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/fanout_heads.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

#include "geom/shape.hpp"
#include "route/bundle.hpp"
#include "route/escape.hpp"

namespace tmk::route {

namespace {

using geom::Point;

std::size_t z(int i) { return static_cast<std::size_t>(i); }

int lowest_layer(model::LayerMask m) {
  for (int l = 0; l < 64; ++l)
    if (m & (model::LayerMask{1} << l)) return l;
  return -1;
}

struct Box {
  Coord x0 = LLONG_MAX, y0 = LLONG_MAX, x1 = LLONG_MIN, y1 = LLONG_MIN;
};
Box pad_box(const model::Pad& p) {
  const auto [hx, hy] = pad_half_extents(p);
  return {p.pos.x - hx, p.pos.y - hy, p.pos.x + hx, p.pos.y + hy};
}
bool inside(const Box& r, Point q) { return r.x0 <= q.x && q.x <= r.x1 && r.y0 <= q.y && q.y <= r.y1; }
// Distance from segment ab to a box (0 when they meet).
long double seg_box_dist(Point a, Point b, const Box& r) {
  if (inside(r, a) || inside(r, b)) return 0;
  const Point c[4] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
  long double d = INFINITY;
  for (int i = 0; i < 4; ++i) d = std::min(d, geom::seg_seg_dist(a, b, c[i], c[(i + 1) % 4]));
  return d;
}

// One pin with a head to plan: its side's outward normal n and tangent t (unit, axis-aligned), its position along t, and the
// turn its destination asks for (-1 toward low t, 0 straight, +1 toward high t).
struct Pin {
  int pad;
  int nx, ny, tx, ty;
  Coord along;
  Point edge;  // the pad's outer end on its centreline
  int want;
};
struct Planned {
  const Pin* pin;
  int turn;
  Coord depth;
};

}  // namespace

std::vector<FanoutHead> plan_fanout_heads(const model::Board& b, const FanoutHeadHooks& h, const FanoutHeadOptions& o) {
  std::vector<FanoutHead> out;
  const auto perim = perimeter_directions(b, BundleOptions{});
  std::set<model::NetId> zone_nets;
  for (const auto& zn : b.zones) zone_nets.insert(zn.net);
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const auto& fp = b.footprints[fi];
    std::vector<int> pins;
    int sides = 0;
    for (int pi : fp.pads)
      if (perim[z(pi)] >= 0) {
        pins.push_back(pi);
        sides |= 1 << (perim[z(pi)] / 2);
      }
    if (static_cast<int>(pins.size()) < o.min_pins || sides != 0xF) continue;
    Box pk;
    for (int pi : pins) {
      const Box r = pad_box(b.pads[z(pi)]);
      pk.x0 = std::min(pk.x0, r.x0), pk.y0 = std::min(pk.y0, r.y0), pk.x1 = std::max(pk.x1, r.x1), pk.y1 = std::max(pk.y1, r.y1);
    }
    const Box ring{pk.x0 - o.reach, pk.y0 - o.reach, pk.x1 + o.reach, pk.y1 + o.reach};
    // Per side (0 +x, 2 +y, 4 -x, 6 -y): the pins whose nearest pad of their net off the package lies beyond the ring.
    std::vector<Pin> side[4];
    for (int pi : pins) {
      const auto& p = b.pads[z(pi)];
      if (p.net <= 0 || zone_nets.count(p.net) || !(p.copper)) continue;
      long double best = INFINITY;
      Point dst{};
      for (std::size_t q = 0; q < b.pads.size(); ++q) {
        const auto& r = b.pads[q];
        if (r.net != p.net || r.footprint == static_cast<int>(fi)) continue;
        const long double d = std::hypot(static_cast<long double>(r.pos.x - p.pos.x), static_cast<long double>(r.pos.y - p.pos.y));
        if (d < best) best = d, dst = r.pos;
      }
      if (best == INFINITY || inside(ring, dst)) continue;
      const int d = perim[z(pi)];
      Pin s{};
      s.pad = pi;
      s.nx = d == 0 ? 1 : d == 4 ? -1 : 0;
      s.ny = d == 2 ? 1 : d == 6 ? -1 : 0;
      s.tx = s.nx ? 0 : 1;
      s.ty = s.nx ? 1 : 0;
      s.along = s.tx ? p.pos.x : p.pos.y;
      const auto [hx, hy] = pad_half_extents(p);
      s.edge = {p.pos.x + s.nx * hx, p.pos.y + s.ny * hy};
      const double dn = static_cast<double>((dst.x - p.pos.x) * s.nx + (dst.y - p.pos.y) * s.ny);
      const double dt = static_cast<double>((dst.x - p.pos.x) * s.tx + (dst.y - p.pos.y) * s.ty);
      s.want = dn > 0 && std::fabs(std::atan2(dt, dn)) < o.turn_deg * M_PI / 180 ? 0 : (dt > 0 ? 1 : -1);
      side[d / 2].push_back(s);
    }
    // Lays one side's heads (turning or all straight); returns the total head length past the pads' outer ends and appends the
    // heads to `made` (their segments are in the copper through hooks.add, handles in `handles`).
    auto lay = [&](std::vector<Pin>& L, bool turns, std::vector<FanoutHead>& made, std::vector<int>& handles) {
      const int n = static_cast<int>(L.size());
      int a = 0, c = n;  // blocks [0,a) turn -1, [a,c) straight, [c,n) turn +1, as many pins as possible turning as they want
      if (turns) {
        int best = -1;
        for (int i = 0; i <= n; ++i)
          for (int j = i; j <= n; ++j) {
            int sc = 0;
            for (int k = 0; k < n; ++k) sc += L[z(k)].want == (k < i ? -1 : k < j ? 0 : 1);
            if (sc > best) best = sc, a = i, c = j;
          }
      }
      std::vector<Planned> plan;
      for (int k = 0; k < n; ++k) {
        const int turn = k < a ? -1 : k < c ? 0 : 1;
        const Coord depth = turn < 0 ? o.bend0 + o.step * k : turn > 0 ? o.bend0 + o.step * (n - 1 - k) : 0;
        plan.push_back({&L[z(k)], turn, depth});
      }
      std::stable_sort(plan.begin(), plan.end(), [](const Planned& x, const Planned& y) {
        return std::make_tuple(x.turn != 0, x.depth, x.pin->pad) < std::make_tuple(y.turn != 0, y.depth, y.pin->pad);
      });
      long double total = 0;
      for (const auto& pl : plan) {
        const auto& p = b.pads[z(pl.pin->pad)];
        const model::NetId net = p.net;
        const int layer = lowest_layer(p.copper);
        const Coord w = h.width(net), cl = h.clearance(net);
        // Other copper of the net is kept at clearance, so the head joins nothing but its own pad.
        auto own_net_clear = [&](Point s, Point e) {
          for (std::size_t q = 0; q < b.pads.size(); ++q)
            if (b.pads[q].net == net && static_cast<int>(q) != pl.pin->pad && (b.pads[q].copper & model::layer_bit(layer)) &&
                seg_box_dist(s, e, pad_box(b.pads[q])) < static_cast<long double>(cl + w / 2))
              return false;
          for (const auto& t : b.tracks)
            if (t.net == net && t.layer == layer && geom::seg_seg_dist(s, e, t.a, t.b) < static_cast<long double>(cl + w / 2 + t.width / 2)) return false;
          for (const auto& v : b.vias)
            if (v.net == net && layer >= v.layer_top && layer <= v.layer_bottom && geom::point_seg_dist(v.pos, s, e) < static_cast<long double>(cl + w / 2 + v.size / 2))
              return false;
          return true;
        };
        auto ok = [&](Point s, Point e) { return h.free(s, e, layer, w, net) && own_net_clear(s, e); };
        const Point pc = p.pos, s0 = pl.pin->edge;
        if (!ok(pc, s0)) continue;
        const double nx = pl.pin->nx, ny = pl.pin->ny;
        struct Leg {
          double ux, uy;
          Coord len;
        };
        std::vector<Leg> legs;
        if (pl.turn == 0) {
          legs.push_back({nx, ny, o.reach});
        } else {
          legs.push_back({nx, ny, pl.depth});
          legs.push_back({(nx + pl.turn * pl.pin->tx) * M_SQRT1_2, (ny + pl.turn * pl.pin->ty) * M_SQRT1_2,
                          static_cast<Coord>(std::llround(static_cast<double>(o.reach - pl.depth) * M_SQRT2))});
        }
        std::vector<Point> pts{pc, s0};
        long double len = 0;
        for (const auto& lg : legs) {
          const Point from = pts.back();
          auto at = [&](Coord t) {
            return Point{from.x + static_cast<Coord>(std::llround(lg.ux * static_cast<double>(t))), from.y + static_cast<Coord>(std::llround(lg.uy * static_cast<double>(t)))};
          };
          // Longest free leg, 25 um resolution: a longer leg covers a shorter one, so the check is monotone in the length.
          constexpr Coord kRes = 25'000;
          Coord lo = 0, hi = lg.len / kRes;
          if (ok(from, at(hi * kRes))) {
            lo = hi;
          } else {
            while (hi - lo > 1) {
              const Coord mid = (lo + hi) / 2;
              (ok(from, at(mid * kRes)) ? lo : hi) = mid;
            }
          }
          const bool cut = lo * kRes < lg.len - kRes;
          Coord got = lo * kRes;
          if (cut) got = std::max<Coord>(0, got - o.back);
          if (got > 0) pts.push_back(at(got));
          len += static_cast<long double>(got);
          if (cut) break;
        }
        if (len < static_cast<long double>(o.min_length)) continue;
        FanoutHead fh{pl.pin->pad, net, layer, pts};
        for (std::size_t k = 0; k + 1 < pts.size(); ++k)
          if (!(pts[k] == pts[k + 1])) handles.push_back(h.add(pts[k], pts[k + 1], layer, w, net));
        made.push_back(std::move(fh));
        total += len;
      }
      return total;
    };
    // Each side either turns toward its destinations or runs straight, whichever lays more head length against what is
    // already there (the designer keeps a side straight where its bundles would run into the parts beside the package).
    for (const int d : {2, 4, 0, 6}) {
      auto& L = side[d / 2];
      if (L.empty()) continue;
      std::stable_sort(L.begin(), L.end(), [](const Pin& x, const Pin& y) { return x.along < y.along; });
      std::vector<FanoutHead> m1, m2;
      std::vector<int> h1, h2;
      const long double lt = lay(L, true, m1, h1);
      for (auto it = h1.rbegin(); it != h1.rend(); ++it) h.remove(*it);
      const long double ls = lay(L, false, m2, h2);
      if (lt > ls) {
        for (auto it = h2.rbegin(); it != h2.rend(); ++it) h.remove(*it);
        m2.clear();
        h2.clear();
        lay(L, true, m2, h2);
      }
      for (auto& fh : m2) out.push_back(std::move(fh));
    }
  }
  return out;
}

}  // namespace tmk::route
