// SPDX-License-Identifier: GPL-3.0-or-later
// The reference-return audit (issue #7, doc 05 section 42): copper under a trace, a void in the reference plane, a split plane, a layer change
// with and without a stitching via or capacitor, the assumed return pad, net selection, and the viewer message. The boards are built by hand
// with known geometry, so the expected detours are computed, not recorded.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <random>

#include "analysis/region.hpp"
#include "analysis/return_path.hpp"
#include "geom/clip.hpp"

using namespace tmk;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

constexpr Coord MM = 1'000'000;

struct Builder {
  model::Board b;
  Builder(int layers = 4) {
    const char* names[] = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"};
    for (int i = 0; i < layers; ++i) {
      model::LayerDef l;
      l.name = i == layers - 1 ? "B.Cu" : names[i];
      l.copper_index = i;
      b.layers.push_back(l);
      b.copper.push_back(i);
    }
    b.nets.push_back({0, "", -1});
  }
  model::NetId net(const std::string& name) {
    for (const auto& n : b.nets)
      if (n.name == name) return n.id;
    const auto id = static_cast<model::NetId>(b.nets.size());
    b.nets.push_back({id, name, -1});
    b.net_index[name] = id;
    return id;
  }
  static geom::Ring rect(double x0, double y0, double x1, double y1, bool ccw = true) {
    geom::Ring r{{static_cast<Coord>(x0 * MM), static_cast<Coord>(y0 * MM)}, {static_cast<Coord>(x1 * MM), static_cast<Coord>(y0 * MM)},
                 {static_cast<Coord>(x1 * MM), static_cast<Coord>(y1 * MM)}, {static_cast<Coord>(x0 * MM), static_cast<Coord>(y1 * MM)}};
    if (!ccw) std::reverse(r.begin(), r.end());
    return r;
  }
  // A filled copper zone on `layer`: the outer ring, and holes (as clockwise rings).
  void zone(const std::string& netname, int layer, const geom::Ring& outer, const std::vector<geom::Ring>& holes = {}) {
    model::Zone z;
    z.net = net(netname);
    z.copper = model::layer_bit(layer);
    z.fills.emplace_back(layer, outer);
    for (const auto& h : holes) z.fills.emplace_back(layer, h);
    b.zones.push_back(std::move(z));
  }
  void track(const std::string& netname, int layer, double x0, double y0, double x1, double y1) {
    model::Track t;
    t.a = {static_cast<Coord>(x0 * MM), static_cast<Coord>(y0 * MM)};
    t.b = {static_cast<Coord>(x1 * MM), static_cast<Coord>(y1 * MM)};
    t.width = 150'000;
    t.layer = layer;
    t.net = net(netname);
    b.tracks.push_back(t);
  }
  void via(const std::string& netname, double x, double y) {
    model::Via v;
    v.pos = {static_cast<Coord>(x * MM), static_cast<Coord>(y * MM)};
    v.size = 300'000;
    v.drill = 150'000;
    v.layer_top = 0;
    v.layer_bottom = b.copper_count() - 1;
    v.net = net(netname);
    b.vias.push_back(v);
  }
  int footprint(const std::string& ref, double x, double y) {
    model::Footprint f;
    f.reference = ref;
    f.pos = {static_cast<Coord>(x * MM), static_cast<Coord>(y * MM)};
    b.footprints.push_back(f);
    return static_cast<int>(b.footprints.size()) - 1;
  }
  void pad(int fp, const std::string& number, const std::string& netname, double x, double y, int layer = 0) {
    model::Pad p;
    p.footprint = fp;
    p.number = number;
    p.net = net(netname);
    p.pos = {static_cast<Coord>(x * MM), static_cast<Coord>(y * MM)};
    p.copper = model::layer_bit(layer);
    b.pads.push_back(p);
    b.footprints[static_cast<std::size_t>(fp)].pads.push_back(static_cast<int>(b.pads.size()) - 1);
  }
};

analysis::ReturnPathOptions select(const std::string& pattern) {
  analysis::ReturnPathOptions o;
  o.name_patterns = {pattern};
  return o;
}

analysis::NetReturn only(const analysis::ReturnPathReport& r) {  // a copy: callers may pass a temporary report
  REQUIRE(r.nets.size() == 1);
  return r.nets[0];
}

}  // namespace

TEST_CASE("return path: copper under the whole trace means no void and no detour", "[return_path]") {
  Builder B;
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40));
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto r = analysis::analyse_return_paths(B.b, rules, select("DDR*"));
  const auto& n = only(r);
  CHECK(n.name == "DDR_D0");
  CHECK_THAT(n.signal_mm, WithinAbs(20.0, 1e-6));
  CHECK(n.void_count == 0);
  CHECK(n.detour_mm == 0);
  CHECK(n.max_separation_mm == 0);
  CHECK(n.uncovered_mm == 0);
  REQUIRE(n.reference_layers.size() == 1);
  CHECK(n.reference_layers[0] == 1);  // the adjacent layer with the copper
  CHECK(n.warnings.empty());
}

TEST_CASE("return path: a rectangular void under the trace forces a measured detour", "[return_path]") {
  Builder B;
  // GND on In1 with a hole 2 mm long and 1 mm wide, centred on the trace (y = 5): the return walks round it.
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40), {Builder::rect(14, 4.5, 16, 5.5, false)});
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  auto opt = select("DDR*");
  opt.warn_detour_mm = 0.5;
  opt.warn_separation_mm = 0.25;
  const auto r = analysis::analyse_return_paths(B.b, rules, opt);
  const auto& n = only(r);
  REQUIRE(n.void_count == 1);
  const auto& v = n.voids[0];
  CHECK_THAT(v.signal_mm, WithinAbs(2.0, 0.01));  // the trace over the hole
  // Entry (14, 5) to exit (16, 5) round a corner of the hole: 0.5 up, 2.0 along, 0.5 down.
  CHECK_THAT(v.return_mm, WithinAbs(3.0, 0.01));
  CHECK_THAT(v.detour_mm, WithinAbs(1.0, 0.01));
  CHECK_THAT(v.max_separation_mm, WithinAbs(0.5, 0.01));
  CHECK_FALSE(v.no_path);
  CHECK(std::llabs(v.entry.x - 14 * MM) < 5'000);
  CHECK(std::llabs(v.exit.x - 16 * MM) < 5'000);
  CHECK(v.path.size() == 4);  // entry, the two corners, exit
  CHECK_THAT(n.detour_mm, WithinAbs(1.0, 0.01));
  CHECK_THAT(n.max_separation_mm, WithinAbs(0.5, 0.01));
  // Both thresholds were set below the measurement: two warnings, and they are warnings (not errors anywhere).
  CHECK(n.warnings.size() == 2);
  CHECK(n.warnings[0].find("detours 1.00 mm") != std::string::npos);
  // The viewer's return path is the trace over copper and the detour over the hole: longer than the trace by the detour.
  REQUIRE(n.ret.size() == 1);
  CHECK(n.ret[0].first == 1);  // drawn on the reference layer
  double len = 0;
  for (std::size_t i = 1; i < n.ret[0].second.size(); ++i)
    len += std::hypot(static_cast<double>(n.ret[0].second[i].x - n.ret[0].second[i - 1].x), static_cast<double>(n.ret[0].second[i].y - n.ret[0].second[i - 1].y));
  CHECK_THAT(len / 1e6, WithinAbs(21.0, 0.02));
}

TEST_CASE("return path: a hole off the trace's line is a smaller detour than one on it", "[return_path]") {
  Builder B;
  // The hole spans y from 4.8 to 5.9: the trace (y = 5) enters near its lower edge, so the way round the near side is 0.2 up.
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40), {Builder::rect(14, 4.8, 16, 5.9, false)});
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto r = analysis::analyse_return_paths(B.b, rules, select("DDR*"));
  const auto v = only(r).voids.at(0);
  CHECK_THAT(v.return_mm, WithinAbs(0.2 + 2.0 + 0.2, 0.01));  // round the lower edge: 0.2 up, 2.0 along, 0.2 down
  CHECK_THAT(v.max_separation_mm, WithinAbs(0.2, 0.01));
}

TEST_CASE("return path: a split plane leaves no return path", "[return_path]") {
  Builder B;
  // Two copper regions with a 0.3 mm slot between them that runs the whole height: nothing connects them.
  B.zone("GND", 1, Builder::rect(0, 0, 19.85, 40));
  B.zone("GND", 1, Builder::rect(20.15, 0, 40, 40));
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto r = analysis::analyse_return_paths(B.b, rules, select("DDR*"));
  const auto& n = only(r);
  REQUIRE(n.void_count == 1);
  CHECK(n.voids[0].no_path);
  CHECK(n.split_count == 1);
  REQUIRE_FALSE(n.warnings.empty());
  CHECK(n.warnings.back().find("split plane") != std::string::npos);
}

TEST_CASE("return path: a stretch over no copper at all is a void, a short one is not", "[return_path]") {
  Builder B;
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40), {Builder::rect(14, -1, 14.04, 41, false)});  // a 0.04 mm slot: below the minimum void
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  CHECK(only(analysis::analyse_return_paths(B.b, rules, select("DDR*"))).void_count == 0);
  auto opt = select("DDR*");
  opt.min_void_mm = 0.01;
  opt.sample_mm = 0.005;  // a slot narrower than the sample spacing can be stepped over: sample finer than it
  CHECK(only(analysis::analyse_return_paths(B.b, rules, opt)).void_count == 1);
}

TEST_CASE("return path: a layer change closes through a stitching via, a capacitor, or is flagged", "[return_path]") {
  const model::DesignRules rules;
  auto board = [&](bool stitch, bool cap, bool second_plane_is_power) {
    Builder B;
    // The signal via has its antipad in both planes (a hole 1 mm square at the via): its own position is a void by design, and the
    // reference net is still the one of the copper around it.
    const geom::Ring antipad = Builder::rect(9.5, 4.5, 10.5, 5.5, false);
    B.zone("GND", 1, Builder::rect(0, 0, 40, 40), {antipad});                                           // F.Cu's reference
    B.zone(second_plane_is_power ? "+3V3" : "GND", 2, Builder::rect(0, 0, 40, 40), {antipad});  // B.Cu's reference
    B.track("DDR_D0", 0, 5, 5, 10, 5);
    B.via("DDR_D0", 10, 5);
    B.track("DDR_D0", 3, 10, 5, 25, 5);
    if (stitch) B.via("GND", 12, 5);
    if (cap) {
      const int c = B.footprint("C7", 8, 8);
      B.pad(c, "1", "GND", 7.5, 8);
      B.pad(c, "2", "+3V3", 8.5, 8);
    }
    return B;
  };
  {  // the same reference on both sides: a stitching via of GND 2 mm away
    const auto B = board(true, false, false);
    const auto& n = only(analysis::analyse_return_paths(B.b, rules, select("DDR*")));
    REQUIRE(n.changes.size() == 1);
    CHECK(n.changes[0].closure == "stitch via");
    CHECK_THAT(n.changes[0].distance_mm, WithinAbs(2.0, 1e-6));
    CHECK(n.changes[0].ref_from == "GND");
    CHECK(n.missing_stitches == 0);
  }
  {  // no stitching via within the radius
    const auto B = board(false, false, false);
    const auto& n = only(analysis::analyse_return_paths(B.b, rules, select("DDR*")));
    CHECK(n.changes[0].closure == "none");
    CHECK(n.missing_stitches == 1);
    CHECK(n.warnings.back().find("without a stitching via or capacitor") != std::string::npos);
  }
  {  // a stitching via beyond the radius does not count
    auto B = board(true, false, false);
    auto opt = select("DDR*");
    opt.stitch_radius_mm = 1.5;
    CHECK(only(analysis::analyse_return_paths(B.b, rules, opt)).missing_stitches == 1);
  }
  {  // two different reference nets: a capacitor between them closes it, a stitching via of one does not
    const auto with_cap = board(false, true, true);
    const auto& n = only(analysis::analyse_return_paths(with_cap.b, rules, select("DDR*")));
    CHECK(n.changes[0].closure == "capacitor");
    CHECK(n.changes[0].ref_to == "+3V3");
    const auto without = board(true, false, true);
    const auto& m = only(analysis::analyse_return_paths(without.b, rules, select("DDR*")));
    CHECK(m.changes[0].closure == "none");
    CHECK(m.missing_stitches == 1);
  }
}

TEST_CASE("return path: the assumed return pad is the nearest pad of the reference net, not a fixed ground ball", "[return_path]") {
  Builder B;
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40));
  const int u = B.footprint("U1", 5, 5);
  B.pad(u, "A1", "DDR_D0", 5, 5);
  B.pad(u, "B1", "GND", 5.8, 5);   // 0.8 mm away
  B.pad(u, "C1", "GND", 5, 5.4);   // 0.4 mm away: the nearer one
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto& n = only(analysis::analyse_return_paths(B.b, rules, select("DDR*")));
  REQUIRE_FALSE(n.ends.empty());
  CHECK(n.ends[0].pad == "U1.A1");
  CHECK(n.ends[0].ref_pad == "U1.C1");
  CHECK_THAT(n.ends[0].distance_mm, WithinAbs(0.4, 1e-6));
}

TEST_CASE("return path: nets are chosen by name, class or pair, and nothing is analysed by default", "[return_path]") {
  Builder B;
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40));
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  B.track("USB_DP", 0, 5, 8, 25, 8);
  B.track("USB_DM", 0, 5, 9, 25, 9);
  B.track("LED", 0, 5, 12, 25, 12);
  model::DesignRules rules;
  CHECK(analysis::analyse_return_paths(B.b, rules, {}).nets.empty());  // off unless asked
  CHECK(analysis::analyse_return_paths(B.b, rules, select("DDR*")).nets.size() == 1);
  CHECK(analysis::analyse_return_paths(B.b, rules, select("USB_*")).nets.size() == 2);
  analysis::ReturnPathOptions pairs;
  pairs.diff_pairs = true;
  pairs.pair_nets = {{B.b.net_by_name("USB_DP"), B.b.net_by_name("USB_DM")}};
  const auto rp = analysis::analyse_return_paths(B.b, rules, pairs);
  REQUIRE(rp.nets.size() == 2);
  CHECK(rp.nets[0].name == "USB_DP");  // in net order
  model::NetClass hs;
  hs.name = "HS";
  rules.classes.push_back(model::NetClass{});
  rules.classes.front().name = "Default";
  rules.classes.push_back(hs);
  rules.assignments["LED"] = {"HS"};
  analysis::ReturnPathOptions by_class;
  by_class.net_classes = {"HS"};
  const auto rc = analysis::analyse_return_paths(B.b, rules, by_class);
  REQUIRE(rc.nets.size() == 1);
  CHECK(rc.nets[0].name == "LED");
}

TEST_CASE("return path: no copper zone, no reference: said so, not a fake clean result", "[return_path]") {
  Builder B(2);
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto r = analysis::analyse_return_paths(B.b, rules, select("DDR*"));
  const auto& n = only(r);
  CHECK_THAT(n.uncovered_mm, WithinAbs(20.0, 1e-6));
  CHECK(n.reference_layers[0] == -1);
  CHECK(n.void_count == 0);
  REQUIRE_FALSE(n.warnings.empty());
  CHECK(n.warnings.back().find("no reference layer") != std::string::npos);
  CHECK(std::any_of(r.notes.begin(), r.notes.end(), [](const std::string& s) { return s.find("no copper zones") != std::string::npos; }));
  // A one-layer board cannot have a reference at all.
  Builder one(1);
  one.track("DDR_D0", 0, 5, 5, 25, 5);
  const auto r1 = analysis::analyse_return_paths(one.b, rules, select("DDR*"));
  CHECK(r1.nets.empty());
  CHECK_FALSE(r1.notes.empty());
}

TEST_CASE("return path: the report and the viewer message carry the numbers and the callouts", "[return_path]") {
  Builder B;
  B.zone("GND", 1, Builder::rect(0, 0, 40, 40), {Builder::rect(14, 4.5, 16, 5.5, false)});
  const int u = B.footprint("U1", 5, 5);
  B.pad(u, "A1", "DDR_D0", 5, 5);
  B.pad(u, "B1", "GND", 5, 5.4);
  B.track("DDR_D0", 0, 5, 5, 25, 5);
  const model::DesignRules rules;
  const auto r = analysis::analyse_return_paths(B.b, rules, select("DDR*"));
  const json j = analysis::return_path_json(B.b, r);
  REQUIRE(j["nets"].size() == 1);
  CHECK(j["nets"][0]["net"] == "DDR_D0");
  CHECK(j["nets"][0]["layers"][0]["layer"] == "F.Cu");
  CHECK(j["nets"][0]["layers"][0]["reference"] == "In1.Cu");
  CHECK_THAT(j["nets"][0]["detour_mm"].get<double>(), WithinAbs(1.0, 0.01));
  CHECK(j["nets"][0]["void_crossings"] == 1);
  CHECK(j["nets"][0]["voids"][0]["no_return_path"] == false);
  const json m = json::parse(analysis::return_path_message(B.b, r.nets[0]));
  CHECK(m["type"] == "return_path");
  CHECK(m["signal"][0]["layer"] == 0);
  CHECK(m["return"][0]["layer"] == 1);
  std::vector<std::string> kinds;
  for (const auto& c : m["callouts"]) kinds.push_back(c["kind"]);
  for (const char* want : {"signal_pad", "ref_pad", "separation"}) CHECK(std::find(kinds.begin(), kinds.end(), want) != kinds.end());
  const auto lines = analysis::return_path_lines(B.b, r);
  REQUIRE_FALSE(lines.empty());
  CHECK(lines[0].find("DDR_D0") == 0);
  CHECK(lines[0].find("F.Cu over In1.Cu") != std::string::npos);
}

TEST_CASE("return path: the indexed region queries equal the linear reference", "[return_path][index]") {
  // A pour of 40 x 30 mm with a field of round antipads, a slot that splits it in two, and an island of copper inside a big hole: the shapes a
  // plane of a real board has. The grid index must give the answers a scan of every edge gives (project rule 3).
  std::mt19937_64 rng(7);
  auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
  geom::Rings holes;
  for (int ix = 0; ix < 15; ++ix)
    for (int iy = 0; iy < 11; ++iy) {
      if (ix >= 11 && ix <= 13 && iy >= 3 && iy <= 8) continue;  // under the big hole
      const geom::Point c{static_cast<Coord>((2.0 + 2.5 * ix + uni(-0.3, 0.3)) * MM), static_cast<Coord>((2.0 + 2.5 * iy + uni(-0.3, 0.3)) * MM)};
      holes.push_back(geom::circle_ring(c, static_cast<Coord>(uni(0.2, 0.9) * MM), 5'000));
    }
  holes.push_back(Builder::rect(28, 8, 36, 22));       // the big hole ...
  holes.push_back(Builder::rect(19.75, -1, 20.25, 31));  // ... and the slot that splits the pour
  geom::Rings pour = geom::subtract({Builder::rect(0, 0, 40, 30)}, holes);
  pour.push_back(Builder::rect(30, 12, 34, 18));  // an island of copper in the big hole
  pour = geom::unite(pour);
  const analysis::detail::Region reg(pour);
  REQUIRE_FALSE(reg.empty());

  int inside = 0, outside = 0, slot = 0;
  for (int i = 0; i < 6000; ++i) {
    const analysis::detail::Pt p{uni(-1.0, 41.0) * MM, uni(-1.0, 31.0) * MM};
    const int fast = reg.island_of(p), slow = reg.island_of_linear(p);
    REQUIRE(fast == slow);
    REQUIRE(reg.contains(p) == (slow >= 0));
    (slow >= 0 ? inside : outside) += 1;
    slot += slow >= 0 && p.x > 19.8 * MM && p.x < 20.2 * MM;
  }
  CHECK(inside > 1000);  // the sampling meets copper and voids both ...
  CHECK(outside > 500);
  CHECK(slot == 0);  // ... and none lies in the slot
  CHECK(reg.island_of({10.0 * MM, 14.0 * MM}) != reg.island_of({30.0 * MM, 28.0 * MM}));  // the slot split the pour
  CHECK(reg.island_of({32.0 * MM, 15.0 * MM}) >= 0);                                     // the island in the hole is copper
  CHECK(reg.island_of({32.0 * MM, 10.0 * MM}) < 0);                                      // and the hole around it is not

  for (int i = 0; i < 3000; ++i) {
    const analysis::detail::Pt a{uni(0, 40) * MM, uni(0, 30) * MM};
    const double ang = uni(0, 6.283185307), len = uni(0.1, 9.0) * MM;
    const analysis::detail::Pt b{a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
    REQUIRE(reg.crosses(a, b) == reg.crosses_linear(a, b));
  }
  for (int i = 0; i < 800; ++i) {
    const analysis::detail::Pt p{uni(-1.0, 41.0) * MM, uni(-1.0, 31.0) * MM};
    const double reach = uni(0.2, 6.0) * MM;
    analysis::detail::Pt q;
    analysis::detail::Pt want;
    const bool found = reg.nearest_boundary(p, reach, q);
    REQUIRE(found == reg.nearest_boundary_linear(p, reach, want));
    if (found) {  // the same point, not only the same distance: ties go to the first edge in both
      REQUIRE(q.x == want.x);
      REQUIRE(q.y == want.y);
    }
  }
  {  // the centre of the island in the big hole is exactly as near to its left and right edges: the tie goes the same way in both
    analysis::detail::Pt q, want;
    const analysis::detail::Pt centre{32.0 * MM, 15.0 * MM};
    REQUIRE(reg.nearest_boundary(centre, 6.0 * MM, q));
    REQUIRE(reg.nearest_boundary_linear(centre, 6.0 * MM, want));
    CHECK(q.x == want.x);
    CHECK(q.y == want.y);
    CHECK_THAT(analysis::detail::dist(centre, q), WithinAbs(2.0 * MM, 1e-6));
  }
  {  // a pad at the centre of a square antipad is as near to all four edges: ties like these, scattered over the grid, must go the same way
    geom::Rings squares;
    std::vector<analysis::detail::Pt> centres;
    for (int i = 0; i < 120; ++i) {
      const double cx = uni(2, 58), cy = uni(2, 28), h = uni(0.2, 0.9);
      const Coord x = static_cast<Coord>(cx * MM), y = static_cast<Coord>(cy * MM), r = static_cast<Coord>(h * MM);
      squares.push_back({{x - r, y - r}, {x - r, y + r}, {x + r, y + r}, {x + r, y - r}});  // clockwise: a hole
      centres.push_back({static_cast<double>(x), static_cast<double>(y)});
    }
    const analysis::detail::Region antipads(geom::subtract({Builder::rect(0, 0, 60, 30)}, geom::unite(squares)));
    int ties = 0;
    for (const auto& c : centres) {
      analysis::detail::Pt q, want;
      const bool found = antipads.nearest_boundary(c, 3.0 * MM, q);
      REQUIRE(found == antipads.nearest_boundary_linear(c, 3.0 * MM, want));
      if (!found) continue;
      ++ties;
      REQUIRE(q.x == want.x);
      REQUIRE(q.y == want.y);
    }
    CHECK(ties > 60);
  }
  for (int i = 0; i < 300; ++i) {
    const analysis::detail::Pt a{uni(0, 40) * MM, uni(0, 30) * MM}, b{uni(0, 40) * MM, uni(0, 30) * MM};
    const double bound = analysis::detail::dist(a, b) + uni(0.1, 6.0) * MM;
    std::vector<analysis::detail::Pt> near;
    reg.vertices_near(a, b, bound, near);
    REQUIRE(near.size() == reg.vertices_near_linear(a, b, bound));
  }
}
