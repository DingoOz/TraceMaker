// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/length.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <utility>

#include "drc/copper.hpp"
#include "geom/shape.hpp"

namespace tmk::drc {

using model::Point;

Coord ViaHeights::between(int a, int b) const {
  if (a > b) std::swap(a, b);
  if (a < 0 || a == b || static_cast<std::size_t>(b) >= to.size()) return 0;
  return to[static_cast<std::size_t>(b)] - from[static_cast<std::size_t>(a)];
}

ViaHeights via_heights(const model::Board& b, const model::DesignRules& rules) {
  ViaHeights vh;
  const int n = b.copper_count();
  if (!rules.use_height_for_length || n < 2) return vh;
  vh.from.assign(static_cast<std::size_t>(n), 0);
  vh.to.assign(static_cast<std::size_t>(n), 0);
  Coord z = 0;
  // An inner layer counts by half (KiCad halves in integer nanometres), an outer one in full.
  auto copper = [&](int ci, Coord th) {
    const bool outer = ci == 0 || ci == n - 1;
    vh.from[static_cast<std::size_t>(ci)] = outer ? z : z + th - th / 2;
    vh.to[static_cast<std::size_t>(ci)] = outer ? z + th : z + th / 2;
    z += th;
  };
  bool listed = b.stackup.present;
  for (int ci = 0; ci < n && listed; ++ci) listed = b.stackup.position_of_copper(ci) >= 0;
  if (listed) {
    for (const auto& l : b.stackup.layers) {
      if (l.is_copper() && l.copper_index >= 0 && l.copper_index < n) copper(l.copper_index, l.thickness);
      else if (l.name.rfind("dielectric", 0) == 0) z += l.thickness;  // mask, silk and paste are not between copper
    }
    return vh;
  }
  // KiCad's default stackup (BOARD_STACKUP::BuildDefaultStackupList).
  const Coord cu = 35'000, mask = 10'000;
  Coord masks = 0;
  for (const auto& l : b.layers)
    if (l.name == "F.Mask" || l.name == "B.Mask") masks += mask;
  const Coord diel = (b.thickness - cu * n - masks) / (n - 1);
  for (int ci = 0; ci < n; ++ci) {
    copper(ci, cu);
    z += diel;
  }
  return vh;
}

namespace {

// One piece of a chain: a straight segment, or a circular arc through `mid`.
struct Piece {
  Point a, b, mid;
  bool arc = false;
};

// KiCad's SHAPE_LINE_CHAIN of one layer, as tracks and arcs merge into it.
struct Chain {
  std::vector<Piece> pieces;
  int layer = -1;
  enum class State { Unmerged, InUse, Retired } state = State::Unmerged;
  Point front() const { return pieces.front().a; }
  Point back() const { return pieces.back().b; }
};

void reverse(std::vector<Piece>& ps) {
  std::reverse(ps.begin(), ps.end());
  for (auto& p : ps) std::swap(p.a, p.b);
}

// Circle through an arc's three points, and the angle it sweeps from a to b through mid (radians, > 0); radius 0
// when the points are collinear.
struct ArcGeom {
  long double cx = 0, cy = 0, r = 0, start = 0, sweep = 0;
  int dir = 1;  // +1 counter-clockwise (y up) from a to b, -1 clockwise
};

ArcGeom arc_geom(const Piece& p) {
  ArcGeom g;
  const long double ax = p.a.x, ay = p.a.y, mx = p.mid.x, my = p.mid.y, bx = p.b.x, by = p.b.y;
  const long double d = 2 * (ax * (my - by) + mx * (by - ay) + bx * (ay - my));
  if (d == 0) return g;
  const long double a2 = ax * ax + ay * ay, m2 = mx * mx + my * my, b2 = bx * bx + by * by;
  g.cx = (a2 * (my - by) + m2 * (by - ay) + b2 * (ay - my)) / d;
  g.cy = (a2 * (bx - mx) + m2 * (ax - bx) + b2 * (mx - ax)) / d;
  g.r = std::hypot(ax - g.cx, ay - g.cy);
  constexpr long double two_pi = 2 * std::numbers::pi_v<long double>;
  auto ang = [&](long double x, long double y) { return std::atan2(y - g.cy, x - g.cx); };
  auto ccw = [&](long double from, long double to) {
    long double t = std::fmod(to - from, two_pi);
    return t < 0 ? t + two_pi : t;
  };
  g.start = ang(ax, ay);
  const long double to_b = ccw(g.start, ang(bx, by)), to_m = ccw(g.start, ang(mx, my));
  if (to_m <= to_b) {
    g.sweep = to_b;
  } else {
    g.sweep = two_pi - to_b;
    g.dir = -1;
  }
  return g;
}

Point arc_at(const ArcGeom& g, long double t) {
  const long double th = g.start + g.dir * t * g.sweep;
  return {std::llround(g.cx + g.r * std::cos(th)), std::llround(g.cy + g.r * std::sin(th))};
}

std::int64_t seg_length(Point a, Point b) {
  return std::llround(std::hypot(static_cast<long double>(b.x - a.x), static_cast<long double>(b.y - a.y)));
}

// SHAPE_LINE_CHAIN::Length: segments rounded to whole nm, then each arc added and truncated.
std::int64_t chain_length(const Chain& c) {
  std::int64_t l = 0;
  for (const auto& p : c.pieces)
    if (!p.arc) l += seg_length(p.a, p.b);
  for (const auto& p : c.pieces) {
    if (!p.arc) continue;
    const ArcGeom g = arc_geom(p);
    const long double len = g.r > 0 ? g.r * g.sweep : static_cast<long double>(seg_length(p.a, p.b));
    l = static_cast<std::int64_t>(static_cast<long double>(l) + len);
  }
  return l;
}

// Point of `p` at parameter t in [0, 1] from a.
Point piece_at(const Piece& p, const ArcGeom& g, long double t) {
  if (p.arc && g.r > 0) return arc_at(g, t);
  return {std::llround(p.a.x + t * static_cast<long double>(p.b.x - p.a.x)), std::llround(p.a.y + t * static_cast<long double>(p.b.y - p.a.y))};
}

// KiCad's clipLineToVia / clipLineToPad, forward: the chain starts inside the shape. The pieces up to the first
// point outside are replaced by a straight line from `centre` to where the chain leaves the shape. A chain wholly
// inside is left as it is.
template <class Inside>
void clip_forward(std::vector<Piece>& ps, Point centre, Inside inside) {
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (inside(ps[i].b)) continue;
    // The chain leaves the shape in piece i, from inside at t = 0 to outside at t = 1: bisection to a nanometre.
    const Piece p = ps[i];
    const ArcGeom g = p.arc ? arc_geom(p) : ArcGeom{};
    long double lo = 0, hi = 1;
    for (int k = 0; k < 64; ++k) {
      const long double mid = (lo + hi) / 2;
      (inside(piece_at(p, g, mid)) ? lo : hi) = mid;
    }
    const Point x = piece_at(p, g, hi);
    std::vector<Piece> out;
    out.push_back({centre, x, {}, false});
    Piece rest = p;
    rest.a = x;
    if (p.arc && g.r > 0) rest.mid = piece_at(p, g, (hi + 1) / 2);
    if (rest.a != rest.b) out.push_back(rest);
    out.insert(out.end(), ps.begin() + static_cast<std::ptrdiff_t>(i) + 1, ps.end());
    ps = std::move(out);
    return;
  }
}

template <class Inside>
void clip_end(Chain& c, Point centre, bool forward, Inside inside) {
  if (forward) {
    clip_forward(c.pieces, centre, inside);
  } else {
    reverse(c.pieces);
    clip_forward(c.pieces, centre, inside);
    reverse(c.pieces);
  }
}

// Point strictly inside a via's disc (SHAPE_CIRCLE::Collide with no clearance).
bool in_via(const model::Via& v, Point p) {
  const long double dx = p.x - v.pos.x, dy = p.y - v.pos.y, r = static_cast<long double>(v.size) / 2;
  return (dx == 0 && dy == 0) || dx * dx + dy * dy < r * r;
}

bool in_pad(const std::vector<geom::Shape>& shapes, Point p) {
  return std::any_of(shapes.begin(), shapes.end(), [&](const geom::Shape& s) { return geom::closer_than_disk(s, p, 0, 1); });
}

std::int64_t layer_delay(const model::Board& b, const model::TuningProfile& tp, int layer) {
  for (const auto& l : tp.layers)
    if (l.layer == b.copper_name(layer)) return l.delay;
  return 0;
}

}  // namespace

NetLength net_length(const model::Board& b, const model::DesignRules& rules, const ViaHeights& vh, const LengthItems& items,
                     const model::TuningProfile* profile) {
  NetLength out;
  const bool use_height = rules.use_height_for_length;
  // Lines in file order, tracks and arcs mixed (copper added since loading last). KiCad takes them in the order of
  // their addresses, which for a loaded board is close to the file's; on a mesh of short tracks the order decides
  // which chains end in a via and are clipped there.
  std::vector<std::pair<sexpr::NodeId, Chain>> ordered;
  for (int i : items.tracks) {
    const auto& t = b.tracks[static_cast<std::size_t>(i)];
    ordered.push_back({t.node, {{{t.a, t.b, {}, false}}, t.layer}});
  }
  for (int i : items.arcs) {
    const auto& a = b.arcs[static_cast<std::size_t>(i)];
    ordered.push_back({a.node, {{{a.a, a.b, a.mid, true}}, a.layer}});
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  std::vector<Chain> lines;
  for (auto& [node, c] : ordered) lines.push_back(std::move(c));
  // Line ends by position (KiCad's linesPositionMap), every layer in one bucket.
  using Key = std::pair<Coord, Coord>;
  std::map<Key, std::vector<int>> ends;
  auto add_end = [&](Point p, int i) {
    auto& v = ends[{p.x, p.y}];
    if (std::find(v.begin(), v.end(), i) == v.end()) v.push_back(i);
  };
  for (std::size_t i = 0; i < lines.size(); ++i) {
    add_end(lines[i].front(), static_cast<int>(i));
    add_end(lines[i].back(), static_cast<int>(i));
  }

  // optimiseVias: a via spans the outermost layers of the lines ending at its centre or inside its pad, and the side
  // of a pad it touches. KiCad takes the first pad its connectivity lists; here the first in board order whose shape
  // holds the via's centre, or whose centre the via holds, on a layer they share.
  std::vector<std::pair<int, int>> via_span(items.vias.size(), {-1, -1});
  for (std::size_t k = 0; k < items.vias.size(); ++k) {
    const auto& v = b.vias[static_cast<std::size_t>(items.vias[k])];
    model::LayerMask span = 0;
    for (const auto& [key, ls] : ends)
      if (const Point p{key.first, key.second}; p == v.pos || in_via(v, p))
        for (int i : ls) span |= model::layer_bit(lines[static_cast<std::size_t>(i)].layer);
    model::LayerMask via_layers = 0;
    for (int l = v.layer_top; l <= v.layer_bottom; ++l) via_layers |= model::layer_bit(l);
    for (const auto& p : b.pads) {
      if (p.net != v.net || !(p.copper & via_layers)) continue;
      if (!in_pad(pad_shapes(p), v.pos) && !in_via(v, p.pos)) continue;
      int side = 0;
      if (p.type == model::PadType::Smd || p.type == model::PadType::Connect) {
        while (side < b.copper_count() - 1 && !(p.copper & model::layer_bit(side))) ++side;
      } else {
        side = p.footprint >= 0 && b.footprints[static_cast<std::size_t>(p.footprint)].back ? b.copper_count() - 1 : 0;
      }
      span |= model::layer_bit(side);
      break;
    }
    if (span == 0) {
      via_span[k] = {v.layer_top, v.layer_top};
      continue;
    }
    int lo = 0, hi = 0;
    for (int l = 0; l < b.copper_count(); ++l)
      if (span & model::layer_bit(l)) hi = l;
    while (!(span & model::layer_bit(lo))) ++lo;
    via_span[k] = {lo, hi};
  }

  // mergeLines (KiCad 10.0.6): a chain grows at an end where exactly one other unmerged line ends, on its layer.
  auto unlist = [&](int i) {
    for (Point p : {lines[static_cast<std::size_t>(i)].front(), lines[static_cast<std::size_t>(i)].back()}) {
      auto& v = ends[{p.x, p.y}];
      v.erase(std::remove(v.begin(), v.end(), i), v.end());
    }
  };
  for (std::size_t pi = 0; pi < lines.size(); ++pi) {
    Chain& primary = lines[pi];
    if (primary.state != Chain::State::Unmerged) continue;
    primary.state = Chain::State::InUse;
    unlist(static_cast<int>(pi));
    for (bool merged = true; merged;) {
      merged = false;
      const Point at[2] = {primary.front(), primary.back()};
      for (int end = 0; end < 2; ++end) {
        const auto it = ends.find({at[end].x, at[end].y});
        if (it == ends.end() || it->second.size() != 1) continue;
        const int other = it->second.front();
        Chain& o = lines[static_cast<std::size_t>(other)];
        if (o.layer != primary.layer) continue;
        o.state = Chain::State::Retired;
        unlist(other);
        std::vector<Piece> ps = o.pieces;
        if (end == 0) {
          if (ps.front().a == primary.front()) reverse(ps);
          ps.insert(ps.end(), primary.pieces.begin(), primary.pieces.end());
          primary.pieces = std::move(ps);
        } else {
          if (ps.front().a != primary.back()) reverse(ps);
          primary.pieces.insert(primary.pieces.end(), ps.begin(), ps.end());
        }
        merged = true;
      }
    }
  }

  // Clip chains that end inside a via (the via's whole disc counts), then chains that end at a pad's centre.
  for (int vi : items.vias) {
    const auto& v = b.vias[static_cast<std::size_t>(vi)];
    auto inside = [&](Point p) { return in_via(v, p); };
    for (auto& c : lines) {
      if (c.state != Chain::State::InUse) continue;
      if (inside(c.front())) clip_end(c, v.pos, true, inside);
      else if (inside(c.back())) clip_end(c, v.pos, false, inside);
    }
  }
  for (int pi : items.pads) {
    const auto& p = b.pads[static_cast<std::size_t>(pi)];
    const auto shapes = pad_shapes(p);
    auto inside = [&](Point q) { return in_pad(shapes, q); };
    for (auto& c : lines) {
      if (c.state != Chain::State::InUse || !(p.copper & model::layer_bit(c.layer))) continue;
      if (c.front() != p.pos && c.back() != p.pos) continue;
      if (inside(c.front())) clip_end(c, p.pos, true, inside);
      else if (inside(c.back())) clip_end(c, p.pos, false, inside);
    }
  }

  for (const auto& c : lines) {
    if (c.state != Chain::State::InUse) continue;
    const std::int64_t len = chain_length(c);
    out.track += len;
    if (profile)
      out.track_delay += static_cast<std::int64_t>(static_cast<double>(layer_delay(b, *profile, c.layer)) * (static_cast<double>(len) / 1e6));
  }
  if (use_height)
    for (std::size_t k = 0; k < items.vias.size(); ++k) {
      const auto& v = b.vias[static_cast<std::size_t>(items.vias[k])];
      const auto [lo, hi] = via_span[k];
      const Coord h = vh.between(lo, hi);
      out.via += h;
      ++out.vias;
      if (!profile || !profile->time_domain) continue;
      // getViaPropagationDelay: an override for these signal and via layers, else the profile's delay per mm of height.
      const auto* ov = profile->via_override(b.copper_name(lo), b.copper_name(hi), b.copper_name(v.layer_top), b.copper_name(v.layer_bottom));
      out.via_delay += ov ? ov->delay : static_cast<std::int64_t>(static_cast<double>(profile->via_delay) * (static_cast<double>(h) / 1e6));
    }
  for (int pi : items.pads) {
    const auto& p = b.pads[static_cast<std::size_t>(pi)];
    out.die += p.die_length;
    ++out.pads;
    if (profile) out.die_delay += p.die_delay;
  }
  return out;
}

}  // namespace tmk::drc
