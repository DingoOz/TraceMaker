// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/octilinear.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "geom/shape.hpp"
#include "route/obstacles.hpp"

namespace tmk::passes {
namespace {

using model::Point;
using geom::i128;

i128 abs128(Coord v) { return v < 0 ? -static_cast<i128>(v) : static_cast<i128>(v); }

// Cosine sign of the corner between an incoming direction u (into the joint) and an outgoing direction v (out of it): the corner is
// acute when the path turns back by more than 90 degrees, i.e. when u . v < 0.
bool acute_turn(Point u, Point v) { return static_cast<i128>(u.x) * v.x + static_cast<i128>(u.y) * v.y < 0; }

Coord sgn(Coord v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

double len(Point a, Point b) { return std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)); }

}  // namespace

bool off_octilinear_segment(Point a, Point b) {
  const Point d{b.x - a.x, b.y - a.y};
  const i128 m = std::min(abs128(d.x), abs128(d.y)), big = std::max(abs128(d.x), abs128(d.y));
  if (m * 10'000'000 < big * 17'453) return false;          // along an axis (within 0.1 degree)
  return (big - m) * 10'000'000 >= (big + m) * 17'453;       // not along a diagonal either
}

OctilinearResult octilinear_tracks(const model::Board& board, const model::DesignRules& rules, std::vector<model::Track>& tracks,
                                   const std::vector<model::Via>& vias, const OctilinearOptions& opt) {
  OctilinearResult res;
  // The router's obstacle model over the board and the new copper, as the widening pass uses it (rule 1).
  model::Board w = board;
  route::Obstacles obs(w, rules);
  obs.set_soft_zones(opt.soft_zones);
  std::vector<int> item(tracks.size(), -1);
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    w.tracks.push_back(tracks[i]);
    item[i] = obs.add_track(static_cast<int>(w.tracks.size()) - 1, static_cast<int>(i));
  }
  for (const auto& v : vias) {
    w.vias.push_back(v);
    obs.add_via(static_cast<int>(w.vias.size()) - 1, 0);
  }
  // Track ends by (net, layer, point): the neighbours a dogleg's legs meet at its two ends.
  using Key = std::tuple<model::NetId, int, Coord, Coord>;
  std::map<Key, std::vector<std::size_t>> at;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    at[{tracks[i].net, tracks[i].layer, tracks[i].a.x, tracks[i].a.y}].push_back(i);
    at[{tracks[i].net, tracks[i].layer, tracks[i].b.x, tracks[i].b.y}].push_back(i);
  }
  std::vector<std::vector<model::Track>> replacement(tracks.size());
  // The direction of track j away from point p (its other end minus p); for a track already replaced, its leg at p.
  auto away = [&](std::size_t j, Point p) {
    const model::Track* t = &tracks[j];
    for (const auto& leg : replacement[j])
      if (leg.a == p || leg.b == p) t = &leg;
    const Point o = t->a == p ? t->b : t->a;
    return Point{o.x - p.x, o.y - p.y};
  };
  // An end at p with first leg direction d (away from p) makes an acute corner with a neighbour when the neighbour, seen from p,
  // points less than 90 degrees away from d: the path comes in along -away and leaves along d.
  auto corner_acute = [&](std::size_t self, Point p, Point d) {
    const auto it = at.find({tracks[self].net, tracks[self].layer, p.x, p.y});
    if (it == at.end()) return false;
    for (const std::size_t j : it->second) {
      if (j == self) continue;
      const Point n = away(j, p);
      if (n.x == 0 && n.y == 0) continue;
      const Point in{-n.x, -n.y};  // coming in from the neighbour
      if (acute_turn(in, d)) return true;
    }
    return false;
  };

  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const auto t = tracks[i];
    if (t.locked || t.a == t.b || !off_octilinear_segment(t.a, t.b)) continue;
    ++res.off_angle;
    if (res.out_of_work) continue;
    const Coord dx = t.b.x - t.a.x, dy = t.b.y - t.a.y;
    const Coord d = std::min(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);  // the diagonal leg's step in x and in y
    const Point diag{sgn(dx) * d, sgn(dy) * d};
    // Two splits: the diagonal first, or the straight leg first.
    const Point m1{t.a.x + diag.x, t.a.y + diag.y}, m2{t.b.x - diag.x, t.b.y - diag.y};
    const double min_leg = opt.min_leg_widths * static_cast<double>(t.width);
    bool any_long = false, any_corner_ok = false, placed = false;
    for (const Point m : {m1, m2}) {
      if (len(t.a, m) < min_leg || len(m, t.b) < min_leg) continue;
      any_long = true;
      if (corner_acute(i, t.a, Point{m.x - t.a.x, m.y - t.a.y}) || corner_acute(i, t.b, Point{m.x - t.b.x, m.y - t.b.y})) continue;
      any_corner_ok = true;
      if (res.checks + 2 > opt.work_limit) {
        res.out_of_work = true;
        break;
      }
      res.checks += 2;
      // The old track is the net's own copper, so it does not block its replacement.
      if (obs.segment_state(t.a, m, t.layer, t.width, t.net, false) != 0 || obs.segment_state(m, t.b, t.layer, t.width, t.net, false) != 0) continue;
      model::Track l1 = t, l2 = t;
      l1.b = m;
      l2.a = m;
      obs.remove_item(item[i]);
      w.tracks.push_back(l1);
      obs.add_track(static_cast<int>(w.tracks.size()) - 1, static_cast<int>(i));
      w.tracks.push_back(l2);
      obs.add_track(static_cast<int>(w.tracks.size()) - 1, static_cast<int>(i));
      item[i] = -1;
      replacement[i] = {l1, l2};
      placed = true;
      ++res.replaced;
      break;
    }
    if (placed || res.out_of_work) continue;
    if (!any_long) ++res.short_leg;
    else if (!any_corner_ok) ++res.acute;
    else ++res.no_room;
  }
  std::vector<model::Track> out;
  out.reserve(tracks.size() + static_cast<std::size_t>(res.replaced));
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (replacement[i].empty()) out.push_back(tracks[i]);
    else out.insert(out.end(), replacement[i].begin(), replacement[i].end());
  }
  tracks = std::move(out);
  return res;
}

}  // namespace tmk::passes
