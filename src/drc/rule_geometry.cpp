// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/rule_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "model/rules.hpp"

namespace tmk::drc {
namespace {
using geom::Point;
using geom::Shape;
using geom::i128;
using Rings = std::vector<std::vector<Point>>;
constexpr Coord kCourtyardError = 5'000;  // shared geom arc polygonization tolerance

bool on_segment(Point p, Point a, Point b) {
  return geom::orient(a, b, p) == 0 && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
         p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

// KiCad can serialize holes as a weakly-simple contour with an out-and-back
// bridge. Those reversed duplicate edges are not material boundaries. Only
// inspect cancellation when an edge would otherwise decide the query.
bool cancelled_edge(const std::vector<Point>& ring, Point a, Point b) {
  for (std::size_t i = 0; i < ring.size(); ++i)
    if (ring[i] == b && ring[(i + 1) % ring.size()] == a) return true;
  return false;
}
template <class F> bool boundaries(const Rings& rings, F&& f) {
  for (const auto& ring : rings)
    for (std::size_t i = 0; i < ring.size(); ++i) {
      const Point a = ring[i], b = ring[(i + 1) % ring.size()];
      if (a != b && f(a, b) && !cancelled_edge(ring, a, b)) return true;
    }
  return false;
}
template <class F> bool edges(const Shape& shape, F&& f) {
  if (shape.pts.size() == 1) return f(shape.pts.front(), shape.pts.front());
  for (std::size_t i = 1; i < shape.pts.size(); ++i)
    if (f(shape.pts[i - 1], shape.pts[i])) return true;
  return shape.closed && !shape.pts.empty() && f(shape.pts.back(), shape.pts.front());
}

// Franklin's ray-crossing test, evaluated at doubled coordinates: interval
// midpoints can lie at half a nanometre and must never be rounded onto a boundary.
// Even/odd filling is independent of ring winding and includes nested holes.
bool contains_twice(const Rings& rings, i128 x, i128 y) {
  bool inside = false;
  for (const auto& ring : rings) {
    for (std::size_t i = 0; i < ring.size(); ++i) {
      const Point a = ring[i], b = ring[(i + 1) % ring.size()];
      const i128 px = x - 2 * static_cast<i128>(a.x), py = y - 2 * static_cast<i128>(a.y);
      const i128 dx = static_cast<i128>(b.x) - a.x, dy = static_cast<i128>(b.y) - a.y;
      const i128 cross = dx * py - dy * px;
      if (cross == 0 && x >= 2 * static_cast<i128>(std::min(a.x, b.x)) &&
          x <= 2 * static_cast<i128>(std::max(a.x, b.x)) &&
          y >= 2 * static_cast<i128>(std::min(a.y, b.y)) &&
          y <= 2 * static_cast<i128>(std::max(a.y, b.y)) && !cancelled_edge(ring, a, b)) return true;
      if ((2 * static_cast<i128>(a.y) > y) != (2 * static_cast<i128>(b.y) > y))
        if (dy > 0 ? cross > 0 : cross < 0) inside = !inside;
    }
  }
  return inside;
}
bool contains(const Rings& rings, Point p) {
  return contains_twice(rings, 2 * static_cast<i128>(p.x), 2 * static_cast<i128>(p.y));
}
bool strict_inside(Point p, const Shape& shape) {
  return !edges(shape, [&](Point a, Point b) { return on_segment(p, a, b); }) &&
         geom::point_in_polygon(p, shape.pts);
}
bool proper_crossing(Point a, Point b, Point c, Point d) {
  const auto opposite = [](i128 x, i128 y) { return (x < 0 && y > 0) || (x > 0 && y < 0); };
  return opposite(geom::orient(a, b, c), geom::orient(a, b, d)) &&
         opposite(geom::orient(c, d, a), geom::orient(c, d, b));
}

bool segment_enclosed(Point a, Point b, const Rings& rings) {
  if (!contains(rings, a) || !contains(rings, b)) return false;
  if (a == b) return true;
  if (boundaries(rings, [&](Point c, Point d) { return proper_crossing(a, b, c, d); })) return false;
  // With proper crossings excluded, the only places membership can change are
  // boundary vertices on ab. Inspect every resulting interval, including edges
  // running along the boundary. Scanning avoids allocating a list of cuts.
  const i128 dx = static_cast<i128>(b.x) - a.x, dy = static_cast<i128>(b.y) - a.y;
  const auto parameter = [&](Point p) {
    return (static_cast<i128>(p.x) - a.x) * dx + (static_cast<i128>(p.y) - a.y) * dy;
  };
  Point current = a;
  i128 at = 0;
  const i128 end = dx * dx + dy * dy;
  while (at < end) {
    Point next = b;
    i128 next_at = end;
    for (const auto& ring : rings)
      for (Point p : ring) {
        if (!on_segment(p, a, b)) continue;
        const i128 t = parameter(p);
        if (t > at && t < next_at) { next = p; next_at = t; }
      }
    if (!contains_twice(rings, static_cast<i128>(current.x) + next.x,
                        static_cast<i128>(current.y) + next.y)) return false;
    current = next;
    at = next_at;
  }
  return true;
}

bool collides(const Shape& shape, const Rings& rings) {
  if (shape.pts.empty() || rings.empty()) return false;
  if (contains(rings, shape.pts.front())) return true;
  if (shape.closed)
    for (const auto& ring : rings)
      if (!ring.empty() && geom::point_in_polygon(ring.front(), shape.pts)) return true;
  return edges(shape, [&](Point a, Point b) {
    return boundaries(rings, [&](Point c, Point d) {
      return geom::segments_intersect(a, b, c, d) ||
             (shape.r > 0 && geom::seg_seg_closer(a, b, c, d, shape.r));
    });
  });
}
bool enclosed(const Shape& shape, const Rings& rings) {
  if (shape.pts.empty() || rings.empty()) return false;
  if (edges(shape, [&](Point a, Point b) { return !segment_enclosed(a, b, rings); })) return false;
  // Edge containment alone misses a hole wholly covered by a filled core. Every
  // region boundary has excluded material on one side; it cannot lie strictly
  // inside a fully enclosed filled polygon.
  if (shape.closed && boundaries(rings, [&](Point a, Point b) {
        return strict_inside(a, shape) || strict_inside(b, shape);
      })) return false;
  return shape.r <= 0 || !edges(shape, [&](Point a, Point b) {
    return boundaries(rings, [&](Point c, Point d) { return geom::seg_seg_closer(a, b, c, d, shape.r); });
  });
}

bool valid_rings(Rings& rings) {
  for (auto& ring : rings) {
    ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
    if (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    if (ring.size() < 3) return false;
    i128 area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
      const Point a = ring[i], b = ring[(i + 1) % ring.size()];
      area += static_cast<i128>(a.x) * b.y - static_cast<i128>(a.y) * b.x;
      for (std::size_t j = i + 1; j < ring.size(); ++j) {
        if (j == i + 1 || (i == 0 && j + 1 == ring.size())) continue;
        if (geom::segments_intersect(a, b, ring[j], ring[(j + 1) % ring.size()])) return false;
      }
    }
    if (area == 0) return false;
  }
  for (std::size_t i = 0; i < rings.size(); ++i)
    for (std::size_t j = i + 1; j < rings.size(); ++j)
      for (std::size_t a = 0; a < rings[i].size(); ++a)
        for (std::size_t b = 0; b < rings[j].size(); ++b)
          if (geom::segments_intersect(rings[i][a], rings[i][(a + 1) % rings[i].size()],
                                       rings[j][b], rings[j][(b + 1) % rings[j].size()])) return false;
  return !rings.empty();
}

CourtyardRegion prepare_courtyard(const model::Board& board, const model::Footprint& fp, int side) {
  CourtyardRegion region;
  std::vector<std::vector<Point>> chains;
  for (int index : fp.graphics) {
    if (index < 0 || static_cast<std::size_t>(index) >= board.graphics.size()) return {};
    const auto& g = board.graphics[static_cast<std::size_t>(index)];
    if (g.layer != (side == 0 ? "F.CrtYd" : "B.CrtYd")) continue;
    switch (g.kind) {
      case model::Graphic::Kind::Line: chains.push_back({g.a, g.b}); break;
      case model::Graphic::Kind::Arc: chains.push_back(geom::arc_points(g.a, g.c, g.b, kCourtyardError)); break;
      case model::Graphic::Kind::Circle: {
        const Coord r = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x),
                                                static_cast<double>(g.b.y - g.a.y)));
        if (r <= 0) return {};
        region.rings.push_back(geom::circle_points(g.a, r, kCourtyardError));
        break;
      }
      case model::Graphic::Kind::Rect:
        region.rings.push_back(g.pts.empty() ? std::vector<Point>{g.a, {g.b.x, g.a.y}, g.b, {g.a.x, g.b.y}} : g.pts);
        break;
      case model::Graphic::Kind::Poly: region.rings.push_back(g.pts); break;
      // The board model retains Bezier controls, not a valid contour. Never
      // manufacture a courtyard by joining its control polygon.
      case model::Graphic::Kind::Curve: return {};
    }
  }
  std::vector<bool> used(chains.size());
  for (std::size_t start = 0; start < chains.size(); ++start) {
    if (used[start]) continue;
    auto ring = std::move(chains[start]);
    used[start] = true;
    if (ring.size() < 2) return {};
    while (ring.front() != ring.back()) {
      std::size_t next = chains.size();
      bool reverse = false;
      for (std::size_t i = 0; i < chains.size(); ++i) {
        if (used[i] || chains[i].empty()) continue;
        if (chains[i].front() == ring.back() || chains[i].back() == ring.back()) {
          // Branching endpoints do not describe an unambiguous closed contour.
          if (next != chains.size()) return {};
          next = i;
          reverse = chains[i].back() == ring.back();
        }
      }
      if (next == chains.size()) return {};
      used[next] = true;
      if (reverse) std::reverse(chains[next].begin(), chains[next].end());
      ring.insert(ring.end(), chains[next].begin() + 1, chains[next].end());
    }
    region.rings.push_back(std::move(ring));
  }
  if (!valid_rings(region.rings)) return {};
  for (const auto& ring : region.rings)
    for (Point p : ring) region.box.add(p);
  return region;
}
}  // namespace

bool area_matches(const CopperItem& item, const model::Zone& area, bool enclosure) {
  if (!(item.layers & area.copper) || item.shapes.empty() || area.outline.empty()) return false;
  for (const auto& ring : area.outline) if (ring.size() < 3) return false;
  if (enclosure) {
    for (const auto& shape : item.shapes) if (!enclosed(shape, area.outline)) return false;
    return true;
  }
  for (const auto& shape : item.shapes) if (collides(shape, area.outline)) return true;
  return false;
}

CourtyardCache build_courtyards(const model::Board& board) {
  CourtyardCache result;
  result.entries.reserve(board.footprints.size());
  for (const auto& fp : board.footprints) {
    CourtyardEntry entry;
    entry.reference = fp.reference;
    entry.lib_id = fp.lib_id;
    entry.back = fp.back;
    entry.sides[0] = prepare_courtyard(board, fp, 0);
    entry.sides[1] = prepare_courtyard(board, fp, 1);
    result.entries.push_back(std::move(entry));
  }
  return result;
}

bool footprint_selected(const std::string& selector, const std::string& reference, const std::string& lib_id) {
  if (selector.empty()) return false;
  return model::wildcard_match(selector, reference) || (selector.find(':') != std::string::npos && model::wildcard_match(selector, lib_id));
}

bool courtyard_matches(const CopperItem& item, const CourtyardCache& courtyards,
                       const std::string& selector, const std::string& function) {
  const bool front = function == "intersectsFrontCourtyard", back = function == "intersectsBackCourtyard";
  if (!front && !back && function != "intersectsCourtyard") return false;
  for (const auto& entry : courtyards.entries) {
    if (!footprint_selected(selector, entry.reference, entry.lib_id)) continue;
    for (int side = 0; side < 2; ++side) {
      // side 0 is F.CrtYd: the footprint's front unless it is flipped.
      const bool own_front = (side == 1) == entry.back;
      if ((front && !own_front) || (back && own_front)) continue;
      const auto& region = entry.sides[static_cast<std::size_t>(side)];
      if (region.rings.empty()) continue;
      for (const auto& shape : item.shapes)
        if (shape.box.intersects(region.box) && collides(shape, region.rings)) return true;
    }
  }
  return false;
}
}  // namespace tmk::drc
