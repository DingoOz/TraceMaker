#include "geom/shape.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tmk::geom {

Shape Shape::point(Point p, Coord r) {
  Shape s;
  s.pts = {p};
  s.r = r;
  s.update_box();
  return s;
}
Shape Shape::segment(Point a, Point b, Coord r) {
  Shape s;
  s.pts = {a, b};
  s.r = r;
  s.update_box();
  return s;
}
Shape Shape::polyline(std::vector<Point> pts, Coord r) {
  Shape s;
  s.pts = std::move(pts);
  s.r = r;
  s.update_box();
  return s;
}
Shape Shape::polygon(std::vector<Point> pts, Coord r) {
  Shape s;
  s.pts = std::move(pts);
  if (s.pts.size() > 1 && s.pts.front() == s.pts.back()) s.pts.pop_back();
  s.r = r;
  s.closed = s.pts.size() >= 3;
  s.update_box();
  return s;
}
void Shape::update_box() {
  box = Box{};
  for (const auto& p : pts) box.add(p);
  box = box.inflated(r);
}

namespace {
inline i128 dot(Point a, Point b, Point c, Point d) {  // (b-a)·(d-c)
  return static_cast<i128>(b.x - a.x) * (d.x - c.x) + static_cast<i128>(b.y - a.y) * (d.y - c.y);
}
inline i128 sq(Point a, Point b) {
  const i128 dx = b.x - a.x, dy = b.y - a.y;
  return dx * dx + dy * dy;
}
inline int sgn(i128 v) { return (v > 0) - (v < 0); }
inline bool on_segment(Point a, Point b, Point p) {  // p collinear with ab: within the box?
  return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
}
}  // namespace

bool segments_intersect(Point a, Point b, Point c, Point d) {
  const int o1 = sgn(orient(a, b, c)), o2 = sgn(orient(a, b, d)), o3 = sgn(orient(c, d, a)), o4 = sgn(orient(c, d, b));
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(a, b, c)) return true;
  if (o2 == 0 && on_segment(a, b, d)) return true;
  if (o3 == 0 && on_segment(c, d, a)) return true;
  if (o4 == 0 && on_segment(c, d, b)) return true;
  return false;
}

bool point_seg_closer(Point p, Point a, Point b, Coord t) {
  const i128 t2 = static_cast<i128>(t) * t;
  const i128 len2 = sq(a, b);
  if (len2 == 0) return sq(a, p) < t2;
  const i128 proj = dot(a, b, a, p);
  if (proj <= 0) return sq(a, p) < t2;
  if (proj >= len2) return sq(b, p) < t2;
  const i128 cr = orient(a, b, p);
  return cr * cr < t2 * len2;
}

bool seg_seg_closer(Point a, Point b, Point c, Point d, Coord t) {
  if (t > 0 && segments_intersect(a, b, c, d)) return true;
  return point_seg_closer(a, c, d, t) || point_seg_closer(b, c, d, t) || point_seg_closer(c, a, b, t) ||
         point_seg_closer(d, a, b, t);
}

long double point_seg_dist(Point p, Point a, Point b) {
  const long double len2 = static_cast<long double>(sq(a, b));
  if (len2 == 0) return std::sqrt(static_cast<long double>(sq(a, p)));
  const long double proj = static_cast<long double>(dot(a, b, a, p));
  if (proj <= 0) return std::sqrt(static_cast<long double>(sq(a, p)));
  if (proj >= len2) return std::sqrt(static_cast<long double>(sq(b, p)));
  return std::fabs(static_cast<long double>(orient(a, b, p))) / std::sqrt(len2);
}

long double seg_seg_dist(Point a, Point b, Point c, Point d) {
  if (segments_intersect(a, b, c, d)) return 0;
  return std::min({point_seg_dist(a, c, d), point_seg_dist(b, c, d), point_seg_dist(c, a, b), point_seg_dist(d, a, b)});
}

bool point_in_polygon(Point p, const std::vector<Point>& poly) {
  bool inside = false;
  const std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Point a = poly[j], b = poly[i];
    if (orient(a, b, p) == 0 && on_segment(a, b, p)) return true;  // on the boundary
    if ((a.y > p.y) != (b.y > p.y)) {
      // x-coordinate of the edge at p.y, compared exactly: p.x < a.x + (p.y-a.y)(b.x-a.x)/(b.y-a.y)
      const i128 lhs = static_cast<i128>(p.x - a.x) * (b.y - a.y);
      const i128 rhs = static_cast<i128>(p.y - a.y) * (b.x - a.x);
      if ((b.y > a.y) ? lhs < rhs : lhs > rhs) inside = !inside;
    }
  }
  return inside;
}

namespace {
// Calls f(a, b) for every edge of the shape's core (a single point gives one degenerate edge).
template <class F>
bool any_edge(const Shape& s, F&& f) {
  const std::size_t n = s.pts.size();
  if (n == 1) return f(s.pts[0], s.pts[0]);
  for (std::size_t i = 0; i + 1 < n; ++i)
    if (f(s.pts[i], s.pts[i + 1])) return true;
  if (s.closed && n >= 3) return f(s.pts[n - 1], s.pts[0]);
  return false;
}
}  // namespace

bool closer_than(const Shape& a, const Shape& b, Coord clearance) {
  const Coord t = clearance + a.r + b.r;  // distance threshold between cores
  if (!a.box.inflated(clearance).intersects(b.box)) return false;
  // Containment: a core point inside the other polygon means the cores overlap.
  if (t > 0) {
    if (a.closed && point_in_polygon(b.pts[0], a.pts)) return true;
    if (b.closed && point_in_polygon(a.pts[0], b.pts)) return true;
  }
  if (t <= 0) {
    // Negative thresholds (deep overlap tests) are not used by DRC; treat as "overlap by more than |t|" = never.
    return false;
  }
  return any_edge(a, [&](Point p, Point q) {
    return any_edge(b, [&](Point u, Point v) { return seg_seg_closer(p, q, u, v, t); });
  });
}

double gap(const Shape& a, const Shape& b) {
  if ((a.closed && point_in_polygon(b.pts[0], a.pts)) || (b.closed && point_in_polygon(a.pts[0], b.pts)))
    return -static_cast<double>(a.r + b.r);
  long double best = 1e30L;
  any_edge(a, [&](Point p, Point q) {
    any_edge(b, [&](Point u, Point v) {
      best = std::min(best, seg_seg_dist(p, q, u, v));
      return false;
    });
    return false;
  });
  return static_cast<double>(best) - static_cast<double>(a.r + b.r);
}

std::vector<Point> arc_points(Point s, Point m, Point e, Coord max_error) {
  // Circle through three points.
  const long double ax = static_cast<long double>(s.x), ay = static_cast<long double>(s.y);
  const long double bx = static_cast<long double>(m.x), by = static_cast<long double>(m.y);
  const long double cx = static_cast<long double>(e.x), cy = static_cast<long double>(e.y);
  const long double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::fabs(d) < 1e-6L) return {s, e};  // collinear: a straight segment
  const long double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
  const long double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
  const long double R = std::hypot(ax - ux, ay - uy);
  long double a0 = std::atan2(ay - uy, ax - ux), a1 = std::atan2(by - uy, bx - ux), a2 = std::atan2(cy - uy, cx - ux);
  // Sweep from a0 to a2 passing through a1.
  auto norm = [](long double x) {
    const long double tau = 2 * std::numbers::pi_v<long double>;
    while (x < 0) x += tau;
    while (x >= tau) x -= tau;
    return x;
  };
  long double sweep = norm(a2 - a0);
  if (norm(a1 - a0) > sweep) sweep -= 2 * std::numbers::pi_v<long double>;  // goes the other way
  const long double err = std::max<long double>(static_cast<long double>(max_error), 1.0L);
  const long double step = R > err ? 2 * std::acos(1 - err / R) : std::numbers::pi_v<long double> / 2;
  const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / step)));
  std::vector<Point> out;
  out.reserve(static_cast<std::size_t>(n) + 1);
  out.push_back(s);
  for (int i = 1; i < n; ++i) {
    const long double ang = a0 + sweep * i / n;
    out.push_back({kiround(static_cast<double>(ux + R * std::cos(ang))), kiround(static_cast<double>(uy + R * std::sin(ang)))});
  }
  out.push_back(e);
  return out;
}

std::vector<Point> circle_points(Point c, Coord radius, Coord max_error) {
  const long double R = static_cast<long double>(radius);
  const long double err = std::max<long double>(static_cast<long double>(max_error), 1.0L);
  const long double step = R > err ? 2 * std::acos(1 - err / R) : std::numbers::pi_v<long double> / 2;
  const int n = std::max(8, static_cast<int>(std::ceil(2 * std::numbers::pi_v<long double> / step)));
  std::vector<Point> out;
  for (int i = 0; i <= n; ++i) {
    const long double ang = 2 * std::numbers::pi_v<long double> * i / n;
    out.push_back({c.x + kiround(static_cast<double>(R * std::cos(ang))), c.y + kiround(static_cast<double>(R * std::sin(ang)))});
  }
  return out;
}

std::vector<Point> convex_hull(std::vector<Point> pts) {
  std::sort(pts.begin(), pts.end(), [](Point a, Point b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  if (pts.size() < 3) return pts;
  std::vector<Point> h(2 * pts.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    while (k >= 2 && orient(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
    h[k++] = pts[i];
  }
  for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && orient(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
    h[k++] = pts[i - 1];
  }
  h.resize(k - 1);
  return h;
}

}  // namespace tmk::geom
