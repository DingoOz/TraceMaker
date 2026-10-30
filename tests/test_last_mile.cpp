// SPDX-License-Identifier: GPL-3.0-or-later
// Finishing the last connections (D155, doc 05 §63): the end-game retry, the pin rescue inside it and the plane fallback are
// deterministic, never lose a routed connection, and change nothing when off.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "geom/shape.hpp"
#include "route/escape.hpp"
#include "route/fanout_heads.hpp"
#include "route/router.hpp"

using namespace tmk;
using geom::Point;

namespace {

const std::string kBoard = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";

bool same_copper(const route::RouteResult& a, const route::RouteResult& b) {
  if (a.routed != b.routed || a.tracks.size() != b.tracks.size() || a.vias.size() != b.vias.size()) return false;
  for (std::size_t i = 0; i < a.tracks.size(); ++i)
    if (a.tracks[i].a != b.tracks[i].a || a.tracks[i].b != b.tracks[i].b || a.tracks[i].layer != b.tracks[i].layer ||
        a.tracks[i].net != b.tracks[i].net)
      return false;
  for (std::size_t i = 0; i < a.vias.size(); ++i)
    if (a.vias[i].pos != b.vias[i].pos || a.vias[i].net != b.vias[i].net) return false;
  return true;
}

}  // namespace

TEST_CASE("last mile: end-game retry and rescue are deterministic and only add connections", "[route][last-mile]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions base;
  base.work_budget = 3'000'000;
  base.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  base.gpu_device = -1;
  base.soft_zones = base.via_in_pad = base.vias_off_pads = true;
  const auto plain = route::Router(b, rules, base).run();

  // Off: the option values that mean "off" give the default run.
  auto off = base;
  off.endgame_frac = 0;
  off.rescue_radii_mm.clear();
  off.plane_informed = false;
  CHECK(same_copper(route::Router(b, rules, off).run(), plain));

  auto on = base;
  on.endgame_frac = 0.2;
  on.rescue_radii_mm = {2, 4, 8};
  on.plane_informed = true;
  const auto r1 = route::Router(b, rules, on).run();
  const auto r2 = route::Router(b, rules, on).run();
  CHECK(same_copper(r1, r2));
  CHECK(r1.endgame_tried == r2.endgame_tried);
  CHECK(r1.rescue_tried == r2.rescue_tried);

  // The retry starts from the best state and keeps a rescue only when it routes more, so it adds without taking away.
  auto eg = base;
  eg.endgame_frac = 0.2;
  const auto e = route::Router(b, rules, eg).run();
  CHECK(e.routed >= plain.routed);
  if (plain.routed < plain.connections) CHECK(e.endgame_tried > 0);
}

TEST_CASE("last mile: via shove, fan-out hold and the negotiation bounds are deterministic and change nothing when off", "[route][last-mile]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions base;
  base.work_budget = 3'000'000;
  base.time_limit_s = 3600;
  base.gpu_device = -1;
  base.soft_zones = base.via_in_pad = base.vias_off_pads = true;
  base.escape_plan = true;
  const auto plain = route::Router(b, rules, base).run();

  // Off: the values that mean "off" give the default run (D156).
  auto off = base;
  off.via_shove = off.fanout_hold = off.pass_bound = off.window_escalate = off.via_retry = off.escape_all = false;
  off.neg_strict_cap = 0;
  off.long_weight = 0;
  off.pin_via_pen_mm = 0;
  CHECK(same_copper(route::Router(b, rules, off).run(), plain));

  auto on = base;
  on.via_shove = on.fanout_hold = on.pass_bound = on.window_escalate = on.via_retry = true;
  on.neg_strict_cap = 200'000;
  on.long_weight = 2;
  on.long_weight_mm = 5;
  on.pin_via_pen_mm = 3;
  const auto r1 = route::Router(b, rules, on).run();
  const auto r2 = route::Router(b, rules, on).run();
  CHECK(same_copper(r1, r2));
  CHECK(r1.via_shoves == r2.via_shoves);
  CHECK(r1.via_shove_trials == r2.via_shove_trials);
  CHECK(r1.window_escalations == r2.window_escalations);
  CHECK(r1.via_retried == r2.via_retried);
  CHECK(r1.fanout_holds == r2.fanout_holds);
}

namespace {

// A 0.5 mm pitch QFN-32 (8 pins a side, pads 0.3 x 0.8 mm) at the origin, every pin on its own net. The left side's pins go to
// pads far down-left, the top's straight up, the right's and bottom's far down-right; pin 1 has a pad 2 mm away (a local
// connection: no head).
model::Board qfn_board() {
  model::Board b;
  b.nets.push_back({0, "", -1});
  model::Footprint fp;
  fp.reference = "U1";
  b.footprints.push_back(fp);
  auto pad = [&](int fpi, const std::string& num, model::NetId net, Coord x, Coord y, Coord sx, Coord sy) {
    model::Pad p;
    p.footprint = fpi;
    p.number = num;
    p.net = net;
    p.pos = {x, y};
    p.size_x = sx, p.size_y = sy;
    p.copper = model::layer_bit(0);
    b.pads.push_back(p);
    if (fpi >= 0) b.footprints[static_cast<std::size_t>(fpi)].pads.push_back(static_cast<int>(b.pads.size() - 1));
  };
  int n = 0;
  for (int side = 0; side < 4; ++side)
    for (int k = 0; k < 8; ++k) {
      ++n;
      b.nets.push_back({static_cast<model::NetId>(n), "N" + std::to_string(n), -1});
      const Coord t = (k - 4) * 500'000 + 250'000;
      const bool horiz = side == 0 || side == 2;  // left/right rows: pads long in x
      const Coord x = side == 0 ? -2'400'000 : side == 2 ? 2'400'000 : t, y = side == 1 ? 2'400'000 : side == 3 ? -2'400'000 : t;
      pad(0, std::to_string(n), static_cast<model::NetId>(n), x, y, horiz ? 800'000 : 300'000, horiz ? 300'000 : 800'000);
    }
  for (int i = 1; i <= n; ++i) {
    const int side = (i - 1) / 8;
    model::Footprint d;
    d.reference = "D" + std::to_string(i);
    b.footprints.push_back(d);
    const Coord off = static_cast<Coord>(i) * 1'000'000;
    const Point q = i == 1 ? Point{-4'400'000, -1'750'000} : side == 0 ? Point{-30'000'000, 20'000'000 + off} : side == 3 ? Point{off, -40'000'000}
                                                                                                          : Point{30'000'000, 30'000'000 + off};
    pad(static_cast<int>(b.footprints.size() - 1), "1", static_cast<model::NetId>(i), q.x, q.y, 600'000, 600'000);
  }
  return b;
}

}  // namespace

TEST_CASE("fan-out heads: planar, deterministic, only the package's own pins, none for a local connection", "[route][last-mile]") {
  const model::Board b = qfn_board();
  constexpr Coord kW = 200'000, kC = 190'000;
  struct Seg {
    Point a, c;
    model::NetId net;
  };
  auto plan = [&](std::vector<Seg>& segs) {
    route::FanoutHeadHooks h;
    h.width = [](model::NetId) { return kW; };
    h.clearance = [](model::NetId) { return kC; };
    h.free = [&](Point a, Point c, int, Coord w, model::NetId n) {  // other nets' heads and pads at track width plus clearance
      for (const auto& s : segs)
        if (s.net != n && geom::seg_seg_dist(a, c, s.a, s.c) < static_cast<long double>(w + kC)) return false;
      for (const auto& p : b.pads) {
        if (p.net == n) continue;
        const auto [hx, hy] = route::pad_half_extents(p);
        const Coord m = kC + w / 2;
        const Point c0{p.pos.x - hx - m, p.pos.y - hy - m}, c1{p.pos.x + hx + m, p.pos.y + hy + m};
        auto in = [&](Point q) { return q.x > c0.x && q.x < c1.x && q.y > c0.y && q.y < c1.y; };
        if (in(a) || in(c) || geom::seg_seg_dist(a, c, c0, {c1.x, c0.y}) == 0 || geom::seg_seg_dist(a, c, {c1.x, c0.y}, c1) == 0 ||
            geom::seg_seg_dist(a, c, c1, {c0.x, c1.y}) == 0 || geom::seg_seg_dist(a, c, {c0.x, c1.y}, c0) == 0)
          return false;
      }
      return true;
    };
    h.add = [&](Point a, Point c, int, Coord, model::NetId n) {
      segs.push_back({a, c, n});
      return static_cast<int>(segs.size() - 1);
    };
    h.remove = [&](int) { segs.pop_back(); };
    route::FanoutHeadOptions o;
    o.reach = 3'000'000;
    return route::plan_fanout_heads(b, h, o);
  };
  std::vector<Seg> s1, s2;
  const auto h1 = plan(s1), h2 = plan(s2);
  REQUIRE(h1.size() == h2.size());
  for (std::size_t i = 0; i < h1.size(); ++i) {
    CHECK(h1[i].pad == h2[i].pad);
    CHECK(h1[i].pts == h2[i].pts);
  }
  // Corner pins are not perimeter pins; pin 1 is local; the rest get a head that starts at its pad's centre.
  CHECK(h1.size() >= 20);
  for (const auto& fh : h1) {
    CHECK(b.pads[static_cast<std::size_t>(fh.pad)].footprint == 0);
    CHECK(b.pads[static_cast<std::size_t>(fh.pad)].number != "1");
    CHECK(fh.pts.front() == b.pads[static_cast<std::size_t>(fh.pad)].pos);
  }
  // Planar at track width plus clearance: no two segments of different nets closer.
  for (std::size_t i = 0; i < s1.size(); ++i)
    for (std::size_t j = i + 1; j < s1.size(); ++j)
      if (s1[i].net != s1[j].net) CHECK(geom::seg_seg_dist(s1[i].a, s1[i].c, s1[j].a, s1[j].c) >= static_cast<long double>(kW + kC));
  // The left side's destinations lie far down-left: its heads bend (a third point past the pad's outer end).
  int bent = 0;
  for (const auto& fh : h1)
    if (b.pads[static_cast<std::size_t>(fh.pad)].pos.x < -2'000'000 && fh.pts.size() == 4) ++bent;
  CHECK(bent >= 3);
}
