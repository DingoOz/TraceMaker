// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/defects.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <tuple>

#include "core/rng.hpp"
#include "geom/shape.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"

namespace tmk::app {

namespace {

using model::Point;

const std::vector<std::string> kKinds = {
    "stub",          // track stub of 1 mm from a track end into the open: one dangling track
    "stub_via",      // the same stub ending in a through via: one via connected on one layer only
    "via_free",      // a via of a track's net 1.5 mm beside it: a via with no connection, one missing connection
    "cut",           // a track removed from the middle of a chain: dangling neighbours, a missing connection
    "shorten",       // a chained track cut back to its midpoint: one dangling end, a missing connection
    "short_cross",   // a track from one net's track across another net's track: crossing centre lines
    "short_touch",   // a track from one net's track into another net's copper without crossing its centre line
    "short_via",     // a via of one net on another net's track
    "short_pad",     // a track from a net's track into a pad of another net
    "short_netless", // a net-less track overlapping a track with a net
    "short_zone",    // a via of one net inside another net's zone fill
    "cross_same",    // a 2 mm track of a track's own net crossing it at its midpoint, ends in the open
};

double len(Point a, Point b) { return std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)); }

Point lerp(Point a, Point b, double t) {
  return {a.x + static_cast<Coord>(std::llround(static_cast<double>(b.x - a.x) * t)),
          a.y + static_cast<Coord>(std::llround(static_cast<double>(b.y - a.y) * t))};
}

// Point `a` moved by `d` nanometres along unit direction (ux, uy).
Point offset(Point a, double ux, double uy, double d) {
  return {a.x + static_cast<Coord>(std::llround(ux * d)), a.y + static_cast<Coord>(std::llround(uy * d))};
}

// Closest point to `p` on segment ab, and the parameter t in [0, 1].
std::pair<Point, double> closest_on(Point a, Point b, Point p) {
  const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
  const double l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? (static_cast<double>(p.x - a.x) * dx + static_cast<double>(p.y - a.y) * dy) / l2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return {lerp(a, b, t), t};
}

class Injector {
 public:
  Injector(io::LoadedBoard& lb, std::uint64_t seed) : lb_(lb), b_(lb.board), ed_(lb, seed), rng_(seed, 0xDEFEu, 0) {}

  int run(const std::string& kind, int count) {
    // Candidate tracks in a seeded order: straight tracks with a net, at least 0.5 mm long.
    for (std::size_t i = 0; i < b_.tracks.size(); ++i) {
      const auto& t = b_.tracks[i];
      if (t.net > 0 && t.layer >= 0 && len(t.a, t.b) >= 500'000) order_.push_back(i);
    }
    std::vector<std::uint64_t> key(b_.tracks.size());
    for (const std::size_t i : order_) key[i] = rng_.u64(i);
    std::sort(order_.begin(), order_.end(), [&](std::size_t x, std::size_t y) { return std::tie(key[x], x) < std::tie(key[y], y); });
    // Endpoint usage per (layer, net, point): a track end shared with another track is "chained".
    for (const auto& t : b_.tracks) {
      ++ends_[{t.layer, t.net, t.a.x, t.a.y}];
      ++ends_[{t.layer, t.net, t.b.x, t.b.y}];
    }
    int done = 0;
    for (const std::size_t i : order_) {
      if (done >= count) break;
      if (one(kind, i)) ++done;
    }
    return done;
  }

  nlohmann::json manifest;

 private:
  bool far_from_others(Point p) const {
    for (const auto& q : used_)
      if (len(p, q) < 5'000'000) return false;  // defects 5 mm apart so their violations do not interact
    return true;
  }
  void record(const std::string& kind, Point p, model::NetId net, const std::string& note) {
    used_.push_back(p);
    manifest.push_back({{"kind", kind},
                        {"pos_mm", {nm_to_mm(p.x), nm_to_mm(p.y)}},
                        {"net", b_.nets[static_cast<std::size_t>(net)].name},
                        {"note", note}});
  }
  bool chained(const model::Track& t) const {
    return ends_.at({t.layer, t.net, t.a.x, t.a.y}) >= 2 && ends_.at({t.layer, t.net, t.b.x, t.b.y}) >= 2;
  }
  model::Via via(Point p, model::NetId net) const {
    return {p, 600'000, 300'000, 0, b_.copper_count() - 1, model::ViaType::Through, net, false, sexpr::kNoNode};
  }
  // Nearest track of another net on the same layer whose centre line is 0.3–3 mm from `p` (index or -1).
  int other_net_track(const model::Track& t, Point p) const {
    int best = -1;
    double bd = 3'000'000;
    for (std::size_t j = 0; j < b_.tracks.size(); ++j) {
      const auto& u = b_.tracks[j];
      if (u.layer != t.layer || u.net == t.net || u.net == 0 || len(u.a, u.b) < 500'000) continue;
      const auto [c, s] = closest_on(u.a, u.b, p);
      const double d = len(p, c);
      if (s <= 0.1 || s >= 0.9 || d < 300'000) continue;  // land on the body of u, away from its ends
      if (d < bd) bd = d, best = static_cast<int>(j);
    }
    return best;
  }

  bool one(const std::string& kind, std::size_t ti) {
    const model::Track& t = b_.tracks[ti];
    const double l = len(t.a, t.b);
    const double ux = static_cast<double>(t.b.x - t.a.x) / l, uy = static_cast<double>(t.b.y - t.a.y) / l;
    const double side = (rng_.u64(1'000'000 + ti) & 1) ? 1.0 : -1.0;
    const double px = -uy * side, py = ux * side;  // unit normal
    const Point mid = lerp(t.a, t.b, 0.5);
    if (!far_from_others(mid)) return false;
    if (kind == "stub" || kind == "stub_via") {
      if (kind == "stub_via" && b_.copper_count() < 2) return false;
      const Point e = offset(t.a, px, py, 1'000'000);
      ed_.add_track({t.a, e, t.width, t.layer, t.net, false, sexpr::kNoNode});
      if (kind == "stub_via") ed_.add_via(via(e, t.net));
      record(kind, e, t.net, "stub from a track end");
      return true;
    }
    if (kind == "via_free") {
      const Point p = offset(mid, px, py, 1'500'000);
      ed_.add_via(via(p, t.net));
      record(kind, p, t.net, "isolated via");
      return true;
    }
    if (kind == "cut" || kind == "shorten") {
      if (!chained(t)) return false;
      ed_.remove_track(ti);
      if (kind == "shorten") ed_.add_track({t.a, mid, t.width, t.layer, t.net, false, sexpr::kNoNode});
      record(kind, mid, t.net, kind == "cut" ? "track removed" : "track cut back to its midpoint");
      return true;
    }
    if (kind == "short_cross" || kind == "short_touch" || kind == "short_netless") {
      const int uj = other_net_track(t, mid);
      if (uj < 0) return false;
      const auto& u = b_.tracks[static_cast<std::size_t>(uj)];
      const Point c = closest_on(u.a, u.b, mid).first;
      const double d = len(mid, c);
      const double dx = static_cast<double>(c.x - mid.x) / d, dy = static_cast<double>(c.y - mid.y) / d;
      Point e;
      if (kind == "short_cross") e = offset(c, dx, dy, 300'000);  // past u's centre line
      // Stop a quarter of the narrower width short of u's centre line: copper overlaps, centre lines do not meet.
      else e = offset(c, dx, dy, -static_cast<double>(std::min(t.width, u.width)) / 4.0);
      const model::NetId net = kind == "short_netless" ? 0 : t.net;
      ed_.add_track({mid, e, t.width, t.layer, net, false, sexpr::kNoNode});
      record(kind, c, t.net, "into a track of net " + b_.nets[static_cast<std::size_t>(u.net)].name);
      return true;
    }
    if (kind == "cross_same") {
      // Its end points lie off the crossed track: linked by copper overlap only, not by an end point.
      const Point a = offset(mid, px, py, -1'000'000), e = offset(mid, px, py, 1'000'000);
      ed_.add_track({a, e, t.width, t.layer, t.net, false, sexpr::kNoNode});
      record(kind, mid, t.net, "crossing its own net");
      return true;
    }
    if (kind == "short_via") {
      const int uj = other_net_track(t, mid);
      if (uj < 0) return false;
      const auto& u = b_.tracks[static_cast<std::size_t>(uj)];
      const Point c = closest_on(u.a, u.b, mid).first;
      ed_.add_via(via(c, t.net));
      record(kind, c, t.net, "via on a track of net " + b_.nets[static_cast<std::size_t>(u.net)].name);
      return true;
    }
    if (kind == "short_pad") {
      // Nearest pad of another net with copper on t's layer, 0.5–4 mm from t's midpoint.
      int best = -1;
      double bd = 4'000'000;
      for (std::size_t j = 0; j < b_.pads.size(); ++j) {
        const auto& p = b_.pads[j];
        if (p.net == 0 || p.net == t.net || !(p.copper & model::layer_bit(t.layer))) continue;
        const double d = len(mid, p.pos);
        if (d >= 500'000 && d < bd) bd = d, best = static_cast<int>(j);
      }
      if (best < 0) return false;
      const auto& p = b_.pads[static_cast<std::size_t>(best)];
      ed_.add_track({mid, p.pos, t.width, t.layer, t.net, false, sexpr::kNoNode});
      record(kind, p.pos, t.net, "into a pad of net " + b_.nets[static_cast<std::size_t>(p.net)].name);
      return true;
    }
    if (kind == "short_zone") {
      // A point inside a zone fill of another net on t's layer, near t; the via carries t's net.
      for (const auto& z : b_.zones) {
        if (z.rule_area || z.net == 0 || z.net == t.net) continue;
        for (const auto& [layer, poly] : z.fills) {
          if (layer != t.layer || poly.size() < 3) continue;
          for (int k = 0; k < 64; ++k) {
            const double a = static_cast<double>(rng_.u64(2'000'000 + ti * 64 + static_cast<std::size_t>(k)) % 6283) / 1000.0;
            const Point p = offset(mid, std::cos(a), std::sin(a), 1'500'000 + 100'000.0 * k);
            // Well inside the fill: the via disk (0.3 mm radius) must not reach the fill's outline.
            if (!geom::point_in_polygon(p, poly)) continue;
            bool clear = true;
            for (std::size_t m = 0; m < poly.size() && clear; ++m)
              if (len(p, closest_on(poly[m], poly[(m + 1) % poly.size()], p).first) < 400'000) clear = false;
            if (!clear || !far_from_others(p)) continue;
            ed_.add_via(via(p, t.net));
            record(kind, p, t.net, "via in a fill of net " + b_.nets[static_cast<std::size_t>(z.net)].name);
            return true;
          }
        }
      }
      return false;
    }
    throw std::invalid_argument("unknown defect kind: " + kind);
  }

  io::LoadedBoard& lb_;
  const model::Board& b_;
  io::BoardEditor ed_;
  RngStream rng_;
  std::vector<std::size_t> order_;
  std::map<std::tuple<int, model::NetId, Coord, Coord>, int> ends_;
  std::vector<Point> used_;

 public:
  void save(const std::string& out) const { ed_.save(out); }
};

}  // namespace

const std::vector<std::string>& defect_kinds() { return kKinds; }

int inject_defects(const std::string& in, const std::string& out, const std::string& manifest, const std::string& kind,
                   std::uint64_t seed, int count) {
  if (std::find(kKinds.begin(), kKinds.end(), kind) == kKinds.end()) throw std::invalid_argument("unknown defect kind: " + kind);
  auto lb = io::read_board_file(in);
  Injector inj(lb, seed);
  inj.manifest = nlohmann::json::array();
  const int n = inj.run(kind, count);
  inj.save(out);
  if (!manifest.empty()) std::ofstream(manifest) << nlohmann::json{{"board", in}, {"kind", kind}, {"seed", seed}, {"defects", inj.manifest}}.dump(1);
  return n;
}

}  // namespace tmk::app
