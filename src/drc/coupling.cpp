// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/coupling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tmk::drc {

using model::Point;

namespace {

__extension__ using i128 = __int128;  // products of two coordinates times a third

// KiCad's rescale (libs/kimath math/util.cpp): a * b / c rounded half away from zero, in 128 bits.
std::int64_t rescale(std::int64_t a, std::int64_t b, std::int64_t c) {
  const i128 num = static_cast<i128>(a) * b;
  return static_cast<std::int64_t>((num < 0) != (c < 0) ? (num - c / 2) / c : (num + c / 2) / c);
}

std::int64_t dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
std::int64_t cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
std::int64_t sq(Point a) { return dot(a, a); }

// SEG::LineProject: the point of the line through a and b nearest p.
Point line_project(Point a, Point b, Point p) {
  const Point d = b - a;
  const std::int64_t l2 = sq(d);
  if (l2 == 0) return a;
  const std::int64_t t = dot(d, p - a);
  return {a.x + rescale(t, d.x, l2), a.y + rescale(t, d.y, l2)};
}

// SEG::NearestPoint: the point of segment a-b nearest p.
Point nearest_point(Point a, Point b, Point p) {
  const Point d = b - a;
  const std::int64_t l2 = sq(d);
  if (l2 == 0) return a;
  const std::int64_t t = dot(d, p - a);
  if (t < 0) return a;
  if (t > l2) return b;
  return {a.x + rescale(t, d.x, l2), a.y + rescale(t, d.y, l2)};
}

}  // namespace

std::optional<Coupling> couple(Point p0, Point p1, Coord wp, Point n0, Point n1, Coord wn) {
  const Point dp = p1 - p0, dn = n1 - n0;
  // Segments of about 1 nm are parallel to everything; KiCad skips them.
  if (sq(dp) <= 2 || sq(dn) <= 2) return std::nullopt;
  // `!ssp.Intersect(ssn, false, true)`: as lines, they must not meet. Parallel lines meet only if collinear. Other lines meet unless
  // the crossing point, as SEG::intersects computes it, does not fit in 32 bits: nearly parallel tracks (vme-wren's DQS0, 10 nm off
  // over 0.2 mm) are coupled.
  const std::int64_t det = cross(dn, dp), q = cross(dp, n0 - p0);
  if (det == 0) {
    if (q == 0) return std::nullopt;
  } else {
    auto at = [&](std::int64_t o, std::int64_t d) {  // VECTOR2L addition, wrapping as the hardware does
      return static_cast<std::int64_t>(static_cast<std::uint64_t>(o) + static_cast<std::uint64_t>(rescale(q, d, det)));
    };
    const std::int64_t x = at(n0.x, dn.x), y = at(n0.y, dn.y);
    constexpr std::int64_t lo = std::numeric_limits<std::int32_t>::min(), hi = std::numeric_limits<std::int32_t>::max();
    if (x >= lo && x <= hi && y >= lo && y <= hi) return std::nullopt;
  }
  // commonParallelProjection: the overlap of the two along P's direction, in TCoef units (|dp|² per segment length).
  const Point proj_a = line_project(p0, p1, n0), proj_b = line_project(p0, p1, n1);
  std::int64_t ta = 0, tb = dot(dp, dp), qa = dot(dp, proj_a - p0), qb = dot(dp, proj_b - p0);
  if (tb < ta) std::swap(ta, tb);
  if (qb < qa) std::swap(qa, qb);
  if (tb <= qa || ta >= qb) return std::nullopt;
  std::int64_t t[4] = {0, dot(dp, dp), qa, qb};
  std::sort(t, t + 4);
  const std::int64_t len2 = sq(dp);
  Coupling c;
  c.pa = {p0.x + rescale(dp.x, t[1], len2), p0.y + rescale(dp.y, t[1], len2)};
  c.pb = {p0.x + rescale(dp.x, t[2], len2), p0.y + rescale(dp.y, t[2], len2)};
  c.na = line_project(n0, n1, c.pa);
  c.nb = line_project(n0, n1, c.pb);
  // SEG::NearestPoints of the two parts (parallel, so they do not intersect): the closest of the four end-to-segment distances.
  const Point cand[4][2] = {{c.pa, nearest_point(c.na, c.nb, c.pa)}, {c.pb, nearest_point(c.na, c.nb, c.pb)},
                            {nearest_point(c.pa, c.pb, c.na), c.na}, {nearest_point(c.pa, c.pb, c.nb), c.nb}};
  int best = 0;
  for (int i = 1; i < 4; ++i)
    if (sq(cand[i][1] - cand[i][0]) < sq(cand[best][1] - cand[best][0])) best = i;
  c.near_p = cand[best][0];
  c.near_n = cand[best][1];
  c.gap = static_cast<Coord>(std::sqrt(static_cast<double>(sq(c.near_n - c.near_p)))) - (wp + wn) / 2;
  c.length = static_cast<Coord>(std::llround(std::sqrt(static_cast<double>(sq(c.nb - c.na)))));
  return c;
}

}  // namespace tmk::drc
