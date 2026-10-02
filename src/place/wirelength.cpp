#include "place/wirelength.hpp"

#include <algorithm>
#include <climits>

namespace tmk::place {

Coord net_hpwl(const Problem& p, const Placement& pl, int net) {
  Coord x0 = LLONG_MAX, x1 = LLONG_MIN, y0 = LLONG_MAX, y1 = LLONG_MIN;
  for (int pi : p.nets[z(net)].pins) {
    const Point q = pl.pin(p, pi);
    x0 = std::min(x0, q.x);
    x1 = std::max(x1, q.x);
    y0 = std::min(y0, q.y);
    y1 = std::max(y1, q.y);
  }
  return (x1 - x0) + (y1 - y0);
}

std::int64_t weighted_hpwl(const Problem& p, const Placement& pl) {
  std::int64_t s = 0;
  for (std::size_t n = 0; n < p.nets.size(); ++n) s += p.nets[n].weight * net_hpwl(p, pl, static_cast<int>(n));
  return s;
}

std::int64_t total_hpwl(const Problem& p, const Placement& pl) {
  std::int64_t s = 0;
  for (std::size_t n = 0; n < p.nets.size(); ++n) s += net_hpwl(p, pl, static_cast<int>(n));
  return s;
}

void net_mst(const std::vector<Point>& pts, int net, std::vector<Seg>& out) {
  const std::size_t n = pts.size();
  if (n < 2) return;
  std::vector<Coord> d(n, LLONG_MAX);
  std::vector<int> from(n, -1);
  std::vector<std::uint8_t> in(n, 0);
  d[0] = 0;
  for (std::size_t it = 0; it < n; ++it) {
    std::size_t best = n;
    for (std::size_t i = 0; i < n; ++i)
      if (!in[i] && (best == n || d[i] < d[best])) best = i;
    in[best] = 1;
    if (from[best] >= 0) out.push_back(Seg{pts[z(from[best])], pts[best], net});
    for (std::size_t i = 0; i < n; ++i) {
      if (in[i]) continue;
      const Coord w = std::llabs(pts[i].x - pts[best].x) + std::llabs(pts[i].y - pts[best].y);
      if (w < d[i]) {
        d[i] = w;
        from[i] = static_cast<int>(best);
      }
    }
  }
}

bool proper_cross(const Seg& s, const Seg& t) {
  if (std::max(s.a.x, s.b.x) < std::min(t.a.x, t.b.x) || std::max(t.a.x, t.b.x) < std::min(s.a.x, s.b.x) ||
      std::max(s.a.y, s.b.y) < std::min(t.a.y, t.b.y) || std::max(t.a.y, t.b.y) < std::min(s.a.y, s.b.y))
    return false;
  const geom::i128 o1 = geom::orient(s.a, s.b, t.a), o2 = geom::orient(s.a, s.b, t.b);
  const geom::i128 o3 = geom::orient(t.a, t.b, s.a), o4 = geom::orient(t.a, t.b, s.b);
  return ((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0));
}

std::vector<Seg> airwires(const Problem& p, const Placement& pl) {
  std::vector<Seg> out;
  std::vector<Point> pts;
  for (std::size_t n = 0; n < p.nets.size(); ++n) {
    if (!p.nets[n].signal) continue;
    pts.clear();
    for (int pi : p.nets[n].pins) pts.push_back(pl.pin(p, pi));
    net_mst(pts, static_cast<int>(n), out);
  }
  return out;
}

std::int64_t count_crossings(const Problem& p, const Placement& pl) {
  const auto w = airwires(p, pl);
  std::int64_t c = 0;
  for (std::size_t i = 0; i < w.size(); ++i)
    for (std::size_t j = i + 1; j < w.size(); ++j)
      if (w[i].net != w[j].net && proper_cross(w[i], w[j])) ++c;
  return c;
}

}  // namespace tmk::place
