// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the placer (tm::place).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>

#include "core/rng.hpp"
#include "crules/engine.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "place/anneal.hpp"
#include "place/global.hpp"
#include "place/legalize.hpp"
#include "place/lower_bound.hpp"
#include "place/placer.hpp"
#include "place/routable.hpp"
#include "place/wirelength.hpp"

using namespace tmk;
using namespace tmk::place;

namespace {

constexpr Coord MM = 1'000'000;

// A synthetic board: rectangular outline [0, w] x [0, h].
Problem board(Coord w, Coord h) {
  Problem p;
  p.outline = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  p.edges.push_back(Shape::polyline({{0, 0}, {w, 0}, {w, h}, {0, h}, {0, 0}}, 0));
  p.region = Box{0, 0, w, h};
  p.clearance = 250'000;
  p.max_need = 200'000;
  // A fixed copper text block in one corner.
  p.fixed_copper.push_back(CopperShape{Shape::polygon({{MM, MM}, {3 * MM, MM}, {3 * MM, 2 * MM}, {MM, 2 * MM}}), 1, 0, 200'000});
  return p;
}

// Adds a part with a rectangular courtyard (half sizes hx, hy around the origin) and pins at `offs`.
int add_part(Problem& p, Point pos, Coord hx, Coord hy, const std::vector<Point>& offs, bool movable, int side = 0) {
  Part pt;
  pt.fp = static_cast<int>(p.parts.size());
  pt.ref = "U" + std::to_string(p.parts.size());
  pt.lib_id = "lib:" + std::to_string(hx) + "x" + std::to_string(hy);
  pt.movable = movable;
  pt.side = side;
  pt.pos0 = pos;
  pt.shape_key = std::hash<std::string>{}(pt.lib_id) * 31u + static_cast<std::uint64_t>(side);
  for (int r = 0; r < 4; ++r) {
    std::vector<Point> c = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
    for (auto& q : c) q = rot90(q, r);
    auto& g = pt.geom[z(r)];
    g.cy[z(side)].push_back(Shape::polygon(c, 0));
    g.body = g.cy[z(side)].back().box;
    g.edge_box = g.body;
    for (const auto& o : offs) {
      g.pads.push_back(Shape::point(rot90(o, r), 100'000));
      g.edge_box.add(g.pads.back().box);
      g.copper.push_back(CopperShape{g.pads.back(), 1, 0, 200'000});  // no net: clearance to every other pad
      g.copper_box.add(g.pads.back().box);
    }
    g.body.add(g.copper_box);
  }
  pt.area = (2 * hx + p.clearance) * (2 * hy + p.clearance);
  p.parts.push_back(pt);
  return static_cast<int>(p.parts.size()) - 1;
}

// Connects (part, pin offset index) pairs into a net.
void add_net(Problem& p, const std::vector<std::pair<int, Point>>& pins, int weight = kSignalWeight) {
  PNet n;
  n.name = "N" + std::to_string(p.nets.size());
  n.weight = weight;
  n.signal = weight == kSignalWeight;
  const int ni = static_cast<int>(p.nets.size());
  for (const auto& [part, off] : pins) {
    Pin q;
    q.part = part;
    q.net = ni;
    for (int r = 0; r < 4; ++r) q.off[z(r)] = rot90(off, r);
    n.pins.push_back(static_cast<int>(p.pins.size()));
    p.parts[z(part)].pins.push_back(static_cast<int>(p.pins.size()));
    p.pins.push_back(q);
  }
  p.nets.push_back(n);
}

// A random synthetic problem: n movable two-pin parts plus 4 fixed anchors, random nets.
Problem random_problem(int n, std::uint64_t seed, Coord size = 40 * MM) {
  Problem p = board(size, size);
  const RngStream rng(seed, 1, 0);
  std::uint64_t k = 0;
  auto u = [&] { return rng.uniform(k++); };
  const Point anchors[4] = {{2 * MM, 2 * MM}, {size - 2 * MM, 2 * MM}, {size - 2 * MM, size - 2 * MM}, {2 * MM, size - 2 * MM}};
  for (const auto& a : anchors) add_part(p, a, 1 * MM, 1 * MM, {{0, 0}}, false);
  for (int i = 0; i < n; ++i) {
    const Coord hx = static_cast<Coord>(0.5e6 + u() * 1.5e6), hy = static_cast<Coord>(0.4e6 + u() * 0.8e6);
    const Point pos{static_cast<Coord>((0.1 + 0.8 * u()) * static_cast<double>(size)), static_cast<Coord>((0.1 + 0.8 * u()) * static_cast<double>(size))};
    add_part(p, pos, hx, hy, {{-hx / 2, 0}, {hx / 2, 0}}, true);
  }
  const int parts = static_cast<int>(p.parts.size());
  for (int e = 0; e < n + 4; ++e) {
    std::vector<std::pair<int, Point>> pins;
    const int deg = 2 + static_cast<int>(u() * 3);
    for (int d = 0; d < deg; ++d) {
      const int part = static_cast<int>(u() * parts) % parts;
      const Part& pt = p.parts[z(part)];
      const Point off = pt.movable ? Point{(u() < 0.5 ? -1 : 1) * (pt.geom[0].cy[0].front().box.x1 / 2), 0} : Point{0, 0};
      pins.emplace_back(part, off);
    }
    add_net(p, pins, e % 7 == 0 ? kPowerWeight : kSignalWeight);
  }
  return p;
}

}  // namespace

TEST_CASE("rot90 matches KiCad rotation", "[place]") {
  const Point q{3, -7};
  for (int r = 0; r < 4; ++r) CHECK(rot90(q, r) == geom::rotate(q, 90.0 * r));
}

TEST_CASE("convex hull", "[place]") {
  const auto h = convex_hull({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 5}, {5, 0}, {0, 0}});
  CHECK(h.size() == 4);
}

TEST_CASE("power-like net names", "[place]") {
  for (const char* s : {"GND", "/power/GNDA", "+3V3", "+5V", "VCC", "VBUS", "3V3", "12V", "-12V", "AGND"}) CHECK(power_like_name(s));
  for (const char* s : {"SDA", "/MCU/PA3", "Net-(R1-Pad2)", "VSYNC_IN", "RESET"}) CHECK_FALSE(power_like_name(s));
}

TEST_CASE("ground-like net names", "[place]") {
  for (const char* s : {"GND", "/power/GNDA", "AGND", "VSS", "0V"}) CHECK(ground_like_name(s));
  for (const char* s : {"+3V3", "VCC", "VBUS", "SDA"}) CHECK_FALSE(ground_like_name(s));
}

TEST_CASE("translated closer() agrees with geom::closer_than", "[place]") {
  const RngStream rng(5, 2, 0);
  std::uint64_t k = 0;
  auto u = [&] { return rng.uniform(k++); };
  for (int it = 0; it < 3000; ++it) {
    std::vector<Point> a, b;
    for (int i = 0; i < 4; ++i) a.push_back({static_cast<Coord>(u() * 4e6), static_cast<Coord>(u() * 4e6)});
    a = place::convex_hull(a);
    if (a.size() < 3) continue;
    b = {{0, 0}, {1'000'000, 0}, {1'000'000, 500'000}, {0, 500'000}};
    const Shape sa = Shape::polygon(a), sb = Shape::polygon(b);
    const Point da{static_cast<Coord>(u() * 2e6), static_cast<Coord>(u() * 2e6)}, db{static_cast<Coord>(u() * 4e6), static_cast<Coord>(u() * 4e6)};
    const Coord c = static_cast<Coord>(u() * 5e5) + 1;
    CHECK(closer(sa, da, sb, db, c) == geom::closer_than(translated(sa, da), translated(sb, db), c));
  }
}

TEST_CASE("min-cost flow on a small graph", "[place]") {
  MinCostFlow g(4);
  g.add_arc(0, 1, 1, 2);
  g.add_arc(0, 2, 5, 2);
  g.add_arc(1, 3, 1, 1);
  g.add_arc(1, 2, -1, 5);
  g.add_arc(2, 3, 2, 5);
  const auto [flow, cost] = g.solve(0, 3, 4);
  CHECK(flow == 4);
  // Two units via 0-1 (one to 3 directly: 2, one via 2: 1-1+2=2), two via 0-2-3: 7 each.
  CHECK(cost == 2 + 2 + 7 + 7);
}

TEST_CASE("HPWL lower bound is exact on a chain and bounds random placements", "[place]") {
  Problem p = board(20 * MM, 20 * MM);
  const int a = add_part(p, {2 * MM, 10 * MM}, MM / 2, MM / 2, {{0, 0}}, false);
  const int b = add_part(p, {12 * MM, 10 * MM}, MM / 2, MM / 2, {{0, 0}}, false);
  const int m = add_part(p, {5 * MM, 3 * MM}, 2 * MM, MM / 2, {{-MM, 0}, {MM, 0}}, true);
  add_net(p, {{a, {0, 0}}, {m, {-MM, 0}}});
  add_net(p, {{m, {MM, 0}}, {b, {0, 0}}});
  Placement pl = Placement::initial(p);
  // The part spans 2 mm of the 10 mm gap: minimum 8 mm (x) + 0 (y), weight 10.
  CHECK(hpwl_lower_bound(p, pl, RotationModel::Fixed) == 8 * MM * kSignalWeight);
  // Any rotation: the bound may only be weaker.
  CHECK(hpwl_lower_bound(p, pl, RotationModel::Any) <= 8 * MM * kSignalWeight);
  // The B2B quadratic optimum reaches it here.
  quadratic_place(p, pl, 8);
  CHECK(std::llabs(weighted_hpwl(p, pl) - 8 * MM * kSignalWeight) < 10'000);

  for (std::uint64_t s = 1; s <= 5; ++s) {
    Problem q = random_problem(25, s);
    Placement ql = Placement::initial(q);
    const auto lb_any = hpwl_lower_bound(q, ql, RotationModel::Any);
    const auto lb_fix = hpwl_lower_bound(q, ql, RotationModel::Fixed);
    CHECK(lb_any <= lb_fix);
    CHECK(lb_fix <= weighted_hpwl(q, ql));
    quadratic_place(q, ql, 8);
    CHECK(lb_fix <= weighted_hpwl(q, ql));
    // Random rotations and positions never beat the any-rotation bound.
    const RngStream rng(s, 3, 0);
    for (std::size_t i = 0; i < q.parts.size(); ++i)
      if (q.parts[i].movable) ql.rot[i] = static_cast<std::uint8_t>(rng.u64(i) & 3);
    CHECK(lb_any <= weighted_hpwl(q, ql));
  }
}

TEST_CASE("raster fast path is conservative (raster free implies exactly legal)", "[place]") {
  Problem p = random_problem(60, 11, 30 * MM);
  Placement pl = Placement::initial(p);
  // Insert half the parts, then probe random candidate positions for the others.
  Legality L(p);
  Raster R(p, 50'000);
  for (std::size_t i = 0; i < p.parts.size(); i += 2) {
    L.insert(static_cast<int>(i), pl.pos[i], pl.rot[i]);
    R.add(static_cast<int>(i), pl.pos[i], pl.rot[i], +1);
  }
  const RngStream rng(3, 4, 0);
  std::uint64_t k = 0;
  int free_count = 0;
  for (int it = 0; it < 20000; ++it) {
    const int part = 1 + 2 * static_cast<int>(rng.u64(k++) % (p.parts.size() / 2));
    if (z(part) >= p.parts.size()) continue;
    const Point q{static_cast<Coord>(rng.uniform(k++) * 30e6), static_cast<Coord>(rng.uniform(k++) * 30e6)};
    const int r = static_cast<int>(rng.u64(k++) & 3);
    const bool f = R.free(part, q, r);
    REQUIRE(f == R.free_reference(part, q, r));  // summed-area tables agree with the cell scan
    if (f) {
      ++free_count;
      REQUIRE(L.legal(part, q, r));
    }
  }
  CHECK(free_count > 100);
}

TEST_CASE("legalisation removes every overlap", "[place]") {
  for (std::uint64_t s = 1; s <= 3; ++s) {
    Problem p = random_problem(80, s, 40 * MM);
    Placement pl = Placement::initial(p);
    const Violations v0 = check_all(p, pl);
    CHECK(v0.overlaps > 0);  // random placement is illegal
    const LegaliseStats st = legalise(p, pl, false);
    CHECK(st.failed == 0);
    const Violations v = check_all(p, pl);
    CHECK(v.overlaps == 0);
    CHECK(v.outside == 0);
  }
}

TEST_CASE("recording a placement never changes it", "[place]") {
  Problem p = random_problem(40, 23, 32 * MM);
  for (const bool tempering : {false, true}) {
    PlaceOptions o;
    o.threads = 3;
    o.runs = 3;
    o.effort = 0.5;
    o.tempering = tempering;
    Placement a = Placement::initial(p), b = Placement::initial(p);
    tmk::place::place(p, a, o);
    std::vector<std::string> stages;
    Placement last;
    o.trace = [&](const std::string& st, const Placement& x, double) {
      stages.push_back(st);
      last = x;
    };
    tmk::place::place(p, b, o);
    CHECK(a.pos == b.pos);
    CHECK(a.rot == b.rot);
    REQUIRE(!stages.empty());
    CHECK(stages.front() == "input");
    CHECK(stages.back() == "placed");
    CHECK(std::count(stages.begin(), stages.end(), "annealing") > 10);
    CHECK(last.pos == b.pos);
  }
}

TEST_CASE("full pipeline: legal, deterministic, incremental cost exact", "[place]") {
  Problem p = random_problem(50, 21, 35 * MM);
  PlaceOptions o;
  o.threads = 4;
  o.runs = 4;
  o.effort = 0.5;
  Placement a = Placement::initial(p), b = Placement::initial(p);
  const PlaceReport ra = tmk::place::place(p, a, o);
  const PlaceReport rb = tmk::place::place(p, b, o);
  CHECK(ra.legal);
  CHECK(ra.after.overlaps == 0);
  CHECK(a.pos == b.pos);
  CHECK(a.rot == b.rot);
  // The annealer's incrementally tracked best cost equals the cost recomputed from scratch.
  CHECK(ra.anneal.cost == anneal_cost(p, a, o.alpha_cross_mm));
  CHECK(ra.after.whpwl >= ra.lb_any_rot);
  // Different thread counts give the same answer (budgets are in moves, runs are independent).
  o.threads = 1;
  Placement c = Placement::initial(p);
  tmk::place::place(p, c, o);
  CHECK(a.pos == c.pos);
  // Refine never makes the annealing cost worse than its legal start.
  o.mode = "refine";
  Placement d = a;
  const PlaceReport rd = tmk::place::place(p, d, o);
  CHECK(rd.legal);
  CHECK(anneal_cost(p, d, o.alpha_cross_mm) <= anneal_cost(p, a, o.alpha_cross_mm));
}

TEST_CASE("rotation descent is monotone", "[place]") {
  Problem p = random_problem(40, 8);
  Placement pl = Placement::initial(p);
  const auto before = weighted_hpwl(p, pl);
  optimise_rotations(p, pl);
  CHECK(weighted_hpwl(p, pl) <= before);
}

TEST_CASE("extraction from a KiCad board", "[place][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const Problem p = extract(lb.board, rules, path);
  CHECK(!p.outline.empty());
  CHECK(p.movable_count() > 10);
  CHECK(!p.nets.empty());
  for (const auto& pt : p.parts) CHECK((!pt.geom[0].cy[0].empty() || !pt.geom[0].cy[1].empty()));
  // Pins at rotation 0 reproduce the absolute pad positions.
  for (const auto& q : p.pins) {
    const Point abs = p.parts[z(q.part)].pos0 + q.off[0];
    bool found = false;
    for (int pi : lb.board.footprints[z(p.parts[z(q.part)].fp)].pads) found |= lb.board.pads[z(pi)].pos == abs;
    CHECK(found);
  }
  // The human placement is a valid input: the bound is below its HPWL.
  const Placement pl = Placement::initial(p);
  CHECK(hpwl_lower_bound(p, pl, RotationModel::Fixed) <= weighted_hpwl(p, pl));
}

TEST_CASE("parts dropped beside the board are placed; parts overhanging the edge stay", "[place]") {
  // R1 on the 20 x 10 mm board, R2/R3 beside it, R4 straddling the right edge.
  auto fp = [](const char* ref, double x, double y) {
    return std::string("  (footprint \"R\" (layer \"F.Cu\") (at ") + std::to_string(x) + " " + std::to_string(y) + ")\n" +
           "    (property \"Reference\" \"" + ref + "\" (at 0 0) (layer \"F.SilkS\"))\n" +
           "    (pad \"1\" smd rect (at -1 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\"))\n" +
           "    (pad \"2\" smd rect (at 1 0) (size 1 1) (layers \"F.Cu\") (net 2 \"B\"))\n  )\n";
  };
  const std::string text = "(kicad_pcb (version 20240108) (generator \"pcbnew\")\n"
                           "  (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal) (37 \"F.SilkS\" user) (44 \"Edge.Cuts\" user))\n"
                           "  (net 0 \"\") (net 1 \"A\") (net 2 \"B\")\n" +
                           fp("R1", 5, 5) + fp("R2", 40, 5) + fp("R3", 60, 20) + fp("R4", 21, 5) +
                           "  (gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\") (stroke (width 0.1) (type solid)))\n)\n";
  const auto dir = std::filesystem::temp_directory_path() / "tmk_place_offboard";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "b.kicad_pcb").string();
  {
    std::ofstream(path) << text;
  }
  const auto lb = io::read_board_file(path);
  const Problem p = extract(lb.board, io::read_design_rules(path), path);
  std::filesystem::remove_all(dir);
  // Off-board pads must not prevent accepting the Edge.Cuts loop.
  REQUIRE(!p.outline.empty());
  auto part = [&](const char* ref) -> const Part& {
    for (const auto& pt : p.parts)
      if (pt.ref == ref) return pt;
    FAIL("no part " << ref);
    return p.parts.front();
  };
  CHECK(part("R1").movable);
  CHECK(part("R2").movable);
  CHECK(part("R3").movable);
  CHECK_FALSE(part("R4").movable);
  CHECK(part("R4").fixed_reason == "overhangs the board edge in the input");
  Placement pl = Placement::initial(p);
  PlaceOptions o;
  o.mode = "full";
  o.runs = 2;
  o.threads = 2;
  o.effort = 0.5;
  const PlaceReport r = tmk::place::place(p, pl, o);
  CHECK(r.legal);
  CHECK(r.legalise_failed == 0);
  for (std::size_t i = 0; i < p.parts.size(); ++i) {
    if (!p.parts[i].movable) continue;
    const Box& body = p.parts[i].geom[pl.rot[i]].body;
    const Point q = pl.pos[i];
    CHECK(q.x + body.x0 >= 0);
    CHECK(q.x + body.x1 <= 20 * MM);
    CHECK(q.y + body.y0 >= 0);
    CHECK(q.y + body.y1 <= 10 * MM);
  }
}

TEST_CASE("decoupling capacitors are tied to an IC supply pin, objective only", "[place][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/ChirpHardware_chirp/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const Problem on = extract(lb.board, rules, path);
  ExtractOptions eo;
  eo.decap_affinity = false;
  const Problem off = extract(lb.board, rules, path, eo);
  int tied = 0;
  for (const auto& n : on.nets) {
    if (!n.affinity) continue;
    ++tied;
    REQUIRE(n.pins.size() == 2);
    CHECK_FALSE(n.signal);
    CHECK(n.weight == kSignalWeight);
    CHECK(on.parts[z(on.pins[z(n.pins[0])].part)].ref.starts_with("C"));
    const std::string ic = on.parts[z(on.pins[z(n.pins[1])].part)].ref;
    CHECK((ic.starts_with("U") || ic.starts_with("IC")));
  }
  CHECK(tied > 0);
  CHECK(on.nets.size() == off.nets.size() + static_cast<std::size_t>(tied));
  // Reported wirelength ignores the pseudo-nets; the objective includes them.
  const Placement a = Placement::initial(on), b = Placement::initial(off);
  CHECK(total_hpwl(on, a) == total_hpwl(off, b));
  CHECK(weighted_hpwl(on, a) > weighted_hpwl(off, b));
  CHECK(count_crossings(on, a) == count_crossings(off, b));
}

TEST_CASE("component-rule proximity pseudo-nets are objective only and skip D25-tied parts", "[place][fixture][crules]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const auto& cat = crules::builtin_catalogue();
  const auto det = crules::detect(lb.board, cat);
  ExtractOptions eo;
  for (const auto& a : crules::placement_affinities(lb.board, cat, det, crules::Mode::Soft)) eo.affinities.push_back({a.pad_a, a.pad_b, a.weight, a.name});
  REQUIRE(!eo.affinities.empty());
  const Problem off = extract(lb.board, rules, path);
  const Problem on = extract(lb.board, rules, path, eo);
  int extra = 0, xtal = 0;
  for (std::size_t n = off.nets.size(); n < on.nets.size(); ++n) {
    const auto& net = on.nets[n];
    REQUIRE(net.affinity);
    CHECK_FALSE(net.signal);
    REQUIRE(net.pins.size() == 2);
    CHECK(on.pins[z(net.pins[0])].part != on.pins[z(net.pins[1])].part);
    // No part D25 already tied is pulled again.
    const std::string& sat = on.parts[z(on.pins[z(net.pins[0])].part)].ref;
    for (const auto& m : off.nets)
      if (m.affinity) CHECK(off.parts[z(off.pins[z(m.pins[0])].part)].ref != sat);
    xtal += net.name.starts_with("~XTAL-") ? 1 : 0;
    ++extra;
  }
  CHECK(extra > 0);
  CHECK(xtal > 0);
  CHECK(extra <= static_cast<int>(eo.affinities.size()));
  const Placement a = Placement::initial(on), b = Placement::initial(off);
  CHECK(total_hpwl(on, a) == total_hpwl(off, b));
  CHECK(count_crossings(on, a) == count_crossings(off, b));
  CHECK(weighted_hpwl(on, a) > weighted_hpwl(off, b));
}

// ---- Connector edge pulls (doc 15 CONN-01, --edge-attraction) ---------------------------------------------------

namespace {

// One-pin pseudo-net from a pin at `off` of `part` to the vertical line x = ax (or horizontal y = ay).
void add_pull(Problem& p, int part, Point off, bool vertical, Coord at, int weight) {
  add_net(p, {{part, off}}, weight);
  PNet& n = p.nets.back();
  n.name = "~pull " + std::to_string(part);
  n.signal = false;
  n.affinity = true;
  if (vertical) n.has_ax = true, n.ax = at;
  else n.has_ay = true, n.ay = at;
}

}  // namespace

TEST_CASE("edge pulls: one-pin anchored nets, exact incremental cost, objective only", "[place][crules]") {
  const Problem base = random_problem(40, 77, 35 * MM);
  Problem p = base;
  std::vector<int> pulled;
  for (std::size_t i = 0; i < p.parts.size() && pulled.size() < 6; ++i)
    if (p.parts[i].movable) pulled.push_back(static_cast<int>(i));
  for (std::size_t k = 0; k < pulled.size(); ++k)  // alternate left edge (x = 0) and top edge (y = 0)
    add_pull(p, pulled[k], {0, 0}, k % 2 == 0, 0, 4 * kSignalWeight);
  // HPWL of a pull = distance of the pin to the line on its axis.
  const Placement s = Placement::initial(p);
  for (std::size_t k = 0; k < pulled.size(); ++k) {
    const int n = static_cast<int>(base.nets.size() + k);
    const Point at = s.pin(p, p.nets[z(n)].pins[0]);
    CHECK(net_hpwl(p, s, n) == (k % 2 == 0 ? at.x : at.y));
  }
  // Reported wirelength and crossings ignore the pulls; the objective counts them.
  CHECK(total_hpwl(p, s) == total_hpwl(base, Placement::initial(base)));
  CHECK(count_crossings(p, s) == count_crossings(base, Placement::initial(base)));
  CHECK(weighted_hpwl(p, s) > weighted_hpwl(base, Placement::initial(base)));
  // The annealer's incremental cost stays exact (independent runs, tempering, LNS) and the result is deterministic.
  PlaceOptions o;
  o.threads = 2;
  o.runs = 2;
  o.effort = 0.5;
  Placement a = Placement::initial(p), b = Placement::initial(p);
  const PlaceReport ra = tmk::place::place(p, a, o);
  tmk::place::place(p, b, o);
  CHECK(ra.legal);
  CHECK(a.pos == b.pos);
  CHECK(ra.anneal.cost == anneal_cost(p, a, o.alpha_cross_mm));
  o.tempering = true;  // (lns_polish runs after the annealer and is checked through lns_improve below)
  Placement t = Placement::initial(p);
  const PlaceReport rt = tmk::place::place(p, t, o);
  CHECK(rt.anneal.cost == anneal_cost(p, t, o.alpha_cross_mm));
  Placement start = Placement::initial(p);
  legalise(p, start, false);
  AnnealOptions ao;
  ao.lns_window = 6;
  ao.exact_window = 3;
  const LnsResult r = lns_improve(p, start, ao, 40);
  CHECK(r.cost_before == anneal_cost(p, start, ao.alpha_cross_mm));
  CHECK(r.cost_after == anneal_cost(p, r.pl, ao.alpha_cross_mm));
  CHECK(r.cost_after <= r.cost_before);
  // The pulls work: the pulled parts end up closer to their edges than without them.
  Placement free_pl = Placement::initial(base);
  o.tempering = false;
  tmk::place::place(base, free_pl, o);
  Coord with = 0, without = 0;
  for (std::size_t k = 0; k < pulled.size(); ++k) {
    with += k % 2 == 0 ? a.pos[z(pulled[k])].x : a.pos[z(pulled[k])].y;
    without += k % 2 == 0 ? free_pl.pos[z(pulled[k])].x : free_pl.pos[z(pulled[k])].y;
  }
  CHECK(with < without);
  // The HPWL lower bound ignores the pulls, so it stays a lower bound of the objective.
  CHECK(ra.after.whpwl >= ra.lb_any_rot);
}

TEST_CASE("edge pulls from component rules: movable connectors only, objective only", "[place][fixture][crules]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const auto& cat = crules::builtin_catalogue();
  const auto det = crules::detect(lb.board, cat);
  CHECK(crules::edge_attractions(lb.board, cat, det, crules::Mode::Report).empty());
  const auto pulls = crules::edge_attractions(lb.board, cat, det, crules::Mode::Soft);
  REQUIRE(!pulls.empty());
  std::set<int> fps;
  for (const auto& e : pulls) {
    CHECK(fps.insert(e.footprint).second);  // once per footprint
    CHECK_FALSE(lb.board.footprints[z(e.footprint)].locked);
    CHECK(lb.board.footprints[z(e.footprint)].lib_id.find("Pin_Header") == std::string::npos);
    CHECK_FALSE(e.body.empty());
  }
  ExtractOptions eo;
  eo.fix_edge_connectors = false;  // make every connector movable so pulls are added
  for (const auto& e : pulls) eo.edge_pulls.push_back({e.footprint, e.body, e.weight, e.name});
  ExtractOptions plain;
  plain.fix_edge_connectors = false;
  const Problem off = extract(lb.board, rules, path, plain);
  const Problem on = extract(lb.board, rules, path, eo);
  int added = 0;
  for (std::size_t n = off.nets.size(); n < on.nets.size(); ++n) {
    const auto& net = on.nets[n];
    CHECK(net.affinity);
    CHECK_FALSE(net.signal);
    CHECK(net.pins.size() == 1);
    CHECK(net.has_ax != net.has_ay);
    CHECK(on.parts[z(on.pins[z(net.pins[0])].part)].movable);
    ++added;
  }
  CHECK(added > 0);
  CHECK(added <= static_cast<int>(pulls.size()));
  const Placement a = Placement::initial(on), b = Placement::initial(off);
  CHECK(total_hpwl(on, a) == total_hpwl(off, b));
  CHECK(count_crossings(on, a) == count_crossings(off, b));
}

// ---- M8: routability term, parallel tempering, LNS, exact windows, ECO, routability loop -----------------------

namespace {

bool all_legal(const Problem& p, const Placement& pl) {
  const Violations v = check_all(p, pl);
  return v.overlaps == 0 && v.outside == 0;
}

// A legal start: the random problem legalised.
Placement legal_start(const Problem& p) {
  Placement pl = Placement::initial(p);
  legalise(p, pl, false);
  return pl;
}

// A synthetic router: each net is one connection, routed when its HPWL is at most `limit`.
RouteFn fake_router(const Problem& p, Coord limit, int* calls = nullptr) {
  return [&p, limit, calls](const Placement& pl) {
    if (calls) ++*calls;
    RouteEval e;
    e.ok = true;
    for (std::size_t n = 0; n < p.nets.size(); ++n) {
      if (p.nets[n].pins.size() < 2) continue;
      ++e.connections;
      if (net_hpwl(p, pl, static_cast<int>(n)) <= limit) {
        ++e.routed;
        continue;
      }
      RouteEval::Failure f;
      f.net = p.nets[n].name;
      f.part_a = p.pins[z(p.nets[n].pins[0])].part;
      f.part_b = p.pins[z(p.nets[n].pins[1])].part;
      f.a = pl.pin(p, p.nets[n].pins[0]);
      f.b = pl.pin(p, p.nets[n].pins[1]);
      e.failed.push_back(f);
    }
    return e;
  };
}

}  // namespace

TEST_CASE("RUDY map: capacity inside the outline, scaling, incremental demand equals the reference", "[place][m8]") {
  Problem p = random_problem(40, 31, 30 * MM);
  CongestionMap m = make_congestion_map(p);
  std::int64_t cap = 0;
  for (auto c : m.cap) cap += c;
  CHECK(cap > 0);
  const Placement pl = legal_start(p);
  const auto d = rudy_demand(p, pl, m);
  std::int64_t total = 0;
  for (auto v : d) total += v;
  CHECK(total > 0);
  // Shrinking capacity can only raise the overflow.
  const auto o0 = rudy_overflow(p, pl, m);
  scale_bins(m, {{15 * MM, 15 * MM}}, 5 * MM, 0.1);
  CHECK(rudy_overflow(p, pl, m) >= o0);
  // The annealer's incremental cost with the routability term equals the from-scratch cost.
  AnnealOptions o;
  o.runs = 2;
  o.threads = 2;
  o.effort = 0.3;
  o.refine = true;
  o.beta_congestion = 2.0;
  o.congestion = &m;
  const AnnealResult r = anneal(p, pl, o);
  CHECK(r.cost == anneal_cost(p, r.pl, o.alpha_cross_mm, o.beta_congestion, &m));
  CHECK(r.overflow == rudy_overflow(p, r.pl, m));
  CHECK(all_legal(p, r.pl));
}

TEST_CASE("parallel tempering: legal, exact cost, identical for any thread count", "[place][m8]") {
  Problem p = random_problem(45, 41, 35 * MM);
  const Placement start = legal_start(p);
  AnnealOptions o;
  o.runs = 4;
  o.effort = 0.2;
  o.tempering = true;
  o.lns_rate = 0.01;
  o.threads = 1;
  const AnnealResult a = anneal(p, start, o);
  o.threads = 4;
  const AnnealResult b = anneal(p, start, o);
  o.threads = 3;
  const AnnealResult c = anneal(p, start, o);
  o.threads = 16;  // more threads than replicas (clamped)
  const AnnealResult d = anneal(p, start, o);
  CHECK(a.pl.pos == d.pl.pos);
  // P2 (doc 14): fixed parts never move.
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (!p.parts[i].movable) CHECK(a.pl.pos[i] == start.pos[i]);
  CHECK(a.pl.pos == b.pl.pos);
  CHECK(a.pl.rot == b.pl.rot);
  CHECK(a.pl.pos == c.pl.pos);
  CHECK(a.cost == b.cost);
  CHECK(a.exchanges_tried > 0);
  CHECK(a.exchanges_accepted == b.exchanges_accepted);
  CHECK(a.lns_tried > 0);
  CHECK(all_legal(p, a.pl));
  CHECK(a.cost == anneal_cost(p, a.pl, o.alpha_cross_mm));
  CHECK(a.cost <= anneal_cost(p, start, o.alpha_cross_mm));
  // The whole pipeline with tempering is deterministic across thread counts too.
  PlaceOptions po;
  po.tempering = true;
  po.runs = 4;
  po.effort = 0.1;
  po.lns_polish = 20;
  po.threads = 1;
  Placement x = Placement::initial(p), y = Placement::initial(p);
  const PlaceReport rx = tmk::place::place(p, x, po);
  po.threads = 4;
  tmk::place::place(p, y, po);
  CHECK(rx.legal);
  CHECK(x.pos == y.pos);
  CHECK(x.rot == y.rot);
}

TEST_CASE("LNS never worsens the cost and keeps the placement legal", "[place][m8]") {
  for (std::uint64_t s = 1; s <= 3; ++s) {
    Problem p = random_problem(40, 50 + s, 30 * MM);
    const Placement start = legal_start(p);
    CongestionMap m = make_congestion_map(p);
    AnnealOptions o;
    o.seed = s;
    o.lns_window = 8;
    o.exact_window = 3;
    o.beta_congestion = s == 2 ? 1.0 : 0.0;
    o.congestion = &m;
    const LnsResult r = lns_improve(p, start, o, 60);
    CHECK(r.tried == 60);
    CHECK(r.cost_after <= r.cost_before);
    CHECK(r.cost_before == anneal_cost(p, start, o.alpha_cross_mm, o.beta_congestion, &m));
    CHECK(r.cost_after == anneal_cost(p, r.pl, o.alpha_cross_mm, o.beta_congestion, &m));
    CHECK(all_legal(p, r.pl));
    if (s == 1) CHECK(r.improved > 0);
    // Deterministic.
    const LnsResult r2 = lns_improve(p, start, o, 60);
    CHECK(r2.pl.pos == r.pl.pos);
  }
}

TEST_CASE("exact window: branch and bound equals full enumeration and never worsens", "[place][m8]") {
  Problem p = random_problem(30, 61, 25 * MM);
  const Placement start = legal_start(p);
  AnnealOptions o;
  int improved = 0;
  for (int seed_part = 4; seed_part < 34; seed_part += 6) {
    std::vector<int> w;
    for (int i = seed_part; i < seed_part + 4 && z(i) < p.parts.size(); ++i) w.push_back(i);
    Placement a = start, b = start;
    const WindowResult ra = solve_window(p, a, w, o, true);
    const WindowResult rb = solve_window(p, b, w, o, false);
    CHECK(ra.proven);
    CHECK(rb.proven);
    CHECK(ra.cost_after == rb.cost_after);  // the bound only prunes, it never changes the optimum
    CHECK(ra.leaves <= rb.leaves);
    CHECK(ra.cost_after <= ra.cost_before);
    CHECK(ra.cost_after == anneal_cost(p, a, o.alpha_cross_mm));
    CHECK(all_legal(p, a));
    // Parts outside the window do not move.
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (std::find(w.begin(), w.end(), static_cast<int>(i)) == w.end()) CHECK(a.pos[i] == start.pos[i]);
    improved += ra.improved ? 1 : 0;
  }
  CHECK(improved > 0);
}

TEST_CASE("ECO: only improving moves, locked parts never move, legal", "[place][m8]") {
  Problem p = random_problem(35, 71, 30 * MM);
  Placement start = legal_start(p);
  // Lock a third of the movable parts (as a KiCad lock would).
  std::vector<int> locked;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable && i % 3 == 0) {
      p.parts[i].movable = false;
      locked.push_back(static_cast<int>(i));
    }
  int calls = 0;
  const RouteFn route = fake_router(p, 12 * MM, &calls);
  const RouteEval e0 = route(start);
  REQUIRE(e0.unrouted() > 0);
  EcoOptions o;
  o.rounds = 4;
  o.candidates = 6;
  const EcoResult r = eco_place(p, start, e0, o, route);
  CHECK(r.eval.unrouted() <= e0.unrouted());
  CHECK(r.committed == static_cast<int>(r.moves.size()));
  if (r.committed > 0) CHECK(r.eval.unrouted() < e0.unrouted());
  CHECK(r.routes <= o.rounds * o.candidates);
  for (int i : locked) {
    CHECK(r.pl.pos[z(i)] == start.pos[z(i)]);
    CHECK(r.pl.rot[z(i)] == start.rot[z(i)]);
  }
  CHECK(all_legal(p, r.pl));
  // The reported result is what the router says about the returned placement.
  CHECK(route(r.pl).unrouted() == r.eval.unrouted());
  // Deterministic.
  const EcoResult r2 = eco_place(p, start, e0, o, route);
  CHECK(r2.pl.pos == r.pl.pos);
  // P8 (doc 14): one round moves at most one part (two for a swap), and only a part near a failed connection.
  o.rounds = 1;
  const EcoResult r1 = eco_place(p, start, e0, o, route);
  std::vector<int> moved;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (r1.pl.pos[i] != start.pos[i] || r1.pl.rot[i] != start.rot[i]) moved.push_back(static_cast<int>(i));
  CHECK(moved.size() <= 2);
  int near_count = 0;
  for (int i : moved) {
    bool near = false;
    const Box& g = p.parts[z(i)].geom[start.rot[z(i)]].body;
    const Box body{g.x0 + start.pos[z(i)].x, g.y0 + start.pos[z(i)].y, g.x1 + start.pos[z(i)].x, g.y1 + start.pos[z(i)].y};
    for (const auto& f : e0.failed) {
      Box fb;
      fb.add(f.a);
      fb.add(f.b);
      near |= f.part_a == i || f.part_b == i || fb.inflated(o.corridor).intersects(body);
    }
    near_count += near ? 1 : 0;
  }
  // A swap partner need not be near (it takes the near part's place); at least one moved part must be.
  if (!moved.empty()) CHECK(near_count >= 1);
}

TEST_CASE("P5: byte-identical board for 1, 3 and 16 threads (tempering + LNS + RUDY, refine)", "[place][m8][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto rules = io::read_design_rules(path);
  std::vector<std::string> out;
  for (int threads : {1, 3, 16}) {
    auto lb = io::read_board_file(path);
    ExtractOptions eo;
    eo.default_clearance = 0;
    const Problem p = extract(lb.board, rules, path, eo);
    Placement pl = Placement::initial(p);
    PlaceOptions o;
    o.mode = "refine";
    o.runs = 4;
    o.threads = threads;
    o.effort = 0.3;
    o.tempering = true;
    o.lns_rate = 0.02;
    o.beta_congestion = 1.0;
    const PlaceReport r = tmk::place::place(p, pl, o);
    CHECK(r.legal);
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (!p.parts[i].movable) CHECK((pl.pos[i] == p.parts[i].pos0 && pl.rot[i] == 0));  // P2
    io::BoardEditor ed(lb, 1);
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (pl.pos[i] != p.parts[i].pos0 || pl.rot[i] != 0)
        ed.move_footprint(static_cast<std::size_t>(p.parts[i].fp), pl.pos[i], p.parts[i].angle0 + 90.0 * pl.rot[i]);
    out.push_back(ed.write());
  }
  CHECK(out[0] == out[1]);
  CHECK(out[0] == out[2]);
}

TEST_CASE("routability loop: never worse than its best seed, locked parts fixed", "[place][m8]") {
  Problem p = random_problem(30, 81, 30 * MM);
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable && i % 4 == 0) p.parts[i].movable = false;
  const Placement start = legal_start(p);
  const RouteFn route = fake_router(p, 10 * MM);
  LoopOptions o;
  o.place.threads = 2;
  o.place.runs = 2;
  o.place.effort = 0.3;
  o.rounds = 2;
  o.eco_candidates = 3;
  std::vector<Candidate> seeds{{"input", start, {}, 0}};
  const LoopResult r = routability_loop(p, seeds, o, route);
  REQUIRE(!r.tried.empty());
  CHECK(r.best.eval.unrouted() <= r.tried.front().eval.unrouted());
  for (const auto& t : r.tried) CHECK(t.eval.ok);
  CHECK(all_legal(p, r.best.pl));
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (!p.parts[i].movable) CHECK(r.best.pl.pos[i] == start.pos[i]);
  CHECK(route(r.best.pl).unrouted() == r.best.eval.unrouted());
}

TEST_CASE("routability loop: illegal seeds are reference only unless the input is accepted as it is", "[place][m8]") {
  Problem p = random_problem(30, 83, 30 * MM);
  const Placement legal = legal_start(p);
  // Overlapping parts give the fake router the shortest wires.
  Placement piled = legal;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable) piled.pos[i] = Point{15 * MM, 15 * MM};
  REQUIRE(measure(p, piled).overlaps > 0);
  const RouteFn route = fake_router(p, 10 * MM);
  REQUIRE(route(piled).unrouted() < route(legal).unrouted());
  LoopOptions o;
  o.place.threads = 2;
  o.place.runs = 2;
  o.place.effort = 0.3;
  o.rounds = 1;
  o.eco_candidates = 2;
  {
    const LoopResult r = routability_loop(p, {{"input", piled, {}, 0}, {"legal", legal, {}, 0}}, o, route);
    REQUIRE(r.tried.size() >= 2);
    CHECK_FALSE(r.tried.front().legal);
    CHECK(r.best.legal);
    CHECK(r.best.label != "input");
    CHECK(all_legal(p, r.best.pl));
  }
  {
    // With no legal seed, return the input without improving it.
    const LoopResult r = routability_loop(p, {{"input", piled, {}, 0}}, o, route);
    CHECK(r.best.label == "input");
    CHECK_FALSE(r.best.legal);
    CHECK(r.rounds == 0);
    CHECK(r.best.pl.pos == piled.pos);
  }
  {
    // The no-repair exception admits the first seed despite its conflicts.
    LoopOptions oa = o;
    oa.accept_first_seed_conflicts = true;
    const LoopResult r = routability_loop(p, {{"input", piled, {}, 0}, {"legal", legal, {}, 0}}, oa, route);
    CHECK(r.best.label == "input");
    CHECK_FALSE(r.best.legal);
  }
}

TEST_CASE("minimal repair: only parts in a conflict move, and the result is legal", "[place][m8]") {
  Problem p = random_problem(30, 85, 30 * MM);
  const Placement legal = legal_start(p);
  // Drop two movable parts onto two others: four parts in conflict, the rest untouched.
  std::vector<int> mov;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable) mov.push_back(static_cast<int>(i));
  REQUIRE(mov.size() >= 4);
  Placement bad = legal;
  bad.pos[z(mov[0])] = legal.pos[z(mov[1])];
  bad.pos[z(mov[2])] = legal.pos[z(mov[3])];
  const Metrics before = measure(p, bad);
  REQUIRE(before.overlaps > 0);
  std::set<std::string> in_conflict;
  for (const auto& c : before.conflicts) {
    const auto slash = c.find('/');
    in_conflict.insert(c.substr(0, slash));
    in_conflict.insert(c.substr(slash + 1));
  }
  int moved = -1;
  const auto rep = repaired(p, bad, &moved);
  REQUIRE(rep.has_value());
  CHECK(all_legal(p, *rep));
  CHECK(moved >= 1);
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (!in_conflict.count(p.parts[i].ref)) {
      CHECK(rep->pos[i] == bad.pos[i]);
      CHECK(rep->rot[i] == bad.rot[i]);
    }
  // Legal input needs no repair.
  const auto same = repaired(p, legal, &moved);
  REQUIRE(same.has_value());
  CHECK(same->pos == legal.pos);
  CHECK(moved == 0);
}

// ---- Side assignment (doc 04 §3 C/E, D48) ----------------------------------------------------------------------

namespace {

ExtractOptions flip_options(const io::LoadedBoard& lb) {
  ExtractOptions eo;
  eo.flip = true;
  eo.flip_ok.assign(lb.board.footprints.size(), 0);
  for (std::size_t i = 0; i < lb.board.footprints.size(); ++i) eo.flip_ok[i] = io::flip_supported(lb, i) ? 1 : 0;
  return eo;
}

bool near(Point a, Point b, Coord tol = 2) { return std::llabs(a.x - b.x) <= tol && std::llabs(a.y - b.y) <= tol; }
bool near(const Box& a, const Box& b, Coord tol = 2) { return near(Point{a.x0, a.y0}, Point{b.x0, b.y0}, tol) && near(Point{a.x1, a.y1}, Point{b.x1, b.y1}, tol); }

}  // namespace

TEST_CASE("flip: the placer's mirrored geometry is what the KiCad writer produces", "[place][fixture][flip]") {
  for (const char* name : {"1Bitsy_1bitsy", "ChirpHardware_chirp"}) {
    const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/" + name + "/unrouted.kicad_pcb";
    if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
    auto lb = io::read_board_file(path);
    const auto rules = io::read_design_rules(path);
    const ExtractOptions eo = flip_options(lb);
    const Problem p = extract(lb.board, rules, path, eo);
    REQUIRE(p.flippable_count() > 5);
    // Every flippable part to a flipped state (all four turns occur) at a shifted origin, written KiCad style.
    Placement pl = Placement::initial(p);
    io::BoardEditor ed(lb, 1);
    int k = 0;
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
      const Part& pt = p.parts[i];
      if (!pt.may_flip()) {
        CHECK(pt.flip_reason.empty() == !pt.movable);  // a movable part that may not flip says why
        continue;
      }
      pl.rot[i] = static_cast<std::uint8_t>(kFlipBit | (k++ & 3));
      pl.pos[i] = pt.pos0 + Point{1'000'000, -2'000'000};
      ed.flip_footprint(static_cast<std::size_t>(pt.fp), pl.pos[i], pt.angle_of(pl.rot[i]));
    }
    // Read back (text round trip) and extract without flipping: each part's input state must equal our state.
    const auto b2 = io::read_board(sexpr::Document::parse(lb.doc.write()));
    const Problem q = extract(b2, rules, path);
    REQUIRE(q.parts.size() == p.parts.size());
    REQUIRE(q.pins.size() == p.pins.size());
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
      const Part& a = p.parts[i];
      const Part& c = q.parts[i];
      if (!flipped(pl.rot[i])) continue;
      CHECK(c.side == 1 - a.side);
      CHECK(c.side == a.side_in(pl.rot[i]));
      CHECK(std::fmod(c.angle0 - a.angle_of(pl.rot[i]) + 720.0, 360.0) < 1e-6);
      const PartGeom& ga = a.geom[pl.rot[i]];
      const PartGeom& gc = c.geom[0];
      for (int s = 0; s < 2; ++s) {
        REQUIRE(ga.cy[z(s)].size() == gc.cy[z(s)].size());
        for (std::size_t j = 0; j < ga.cy[z(s)].size(); ++j) CHECK(near(ga.cy[z(s)][j].box, gc.cy[z(s)][j].box));
      }
      REQUIRE(ga.pads.size() == gc.pads.size());
      for (std::size_t j = 0; j < ga.pads.size(); ++j) CHECK(near(ga.pads[j].box, gc.pads[j].box));
      REQUIRE(ga.copper.size() == gc.copper.size());
      for (std::size_t j = 0; j < ga.copper.size(); ++j) {
        CHECK(near(ga.copper[j].s.box, gc.copper[j].s.box));
        CHECK(ga.copper[j].layers == gc.copper[j].layers);
      }
    }
    const Placement qi = Placement::initial(q);
    for (std::size_t j = 0; j < p.pins.size(); ++j)
      // Real pads only: decoupling ties pick the nearest IC pad of the input, which moved.
      if (flipped(pl.rot[z(p.pins[j].part)]) && !p.nets[z(p.pins[j].net)].affinity) {
        const Point u = pl.pin(p, static_cast<int>(j)), v = qi.pin(q, static_cast<int>(j));
        INFO(name << " " << p.parts[z(p.pins[j].part)].ref << " net " << p.nets[z(p.pins[j].net)].name << " " << u.x << "," << u.y << " vs " << v.x << "," << v.y);
        CHECK(near(u, v));
      }
  }
}

TEST_CASE("flip: legal, exact incremental cost with the via term, deterministic, only allowed parts flip", "[place][fixture][flip]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/ChirpHardware_chirp/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  ExtractOptions eo = flip_options(lb);
  const Problem p0 = extract(lb.board, rules, path, eo);
  std::string pinned;
  for (const auto& pt : p0.parts)
    if (pt.may_flip()) pinned = pt.ref;  // the user pins one part to its side (--keep-side)
  eo.keep_side = {pinned};
  const Problem p = extract(lb.board, rules, path, eo);
  std::vector<Placement> res;
  for (int threads : {1, 3}) {
    PlaceOptions o;
    o.mode = "full";
    o.runs = 3;
    o.threads = threads;
    o.effort = 0.3;
    o.flip = true;
    Placement pl = Placement::initial(p);
    const PlaceReport r = tmk::place::place(p, pl, o);
    CHECK(r.legal);
    CHECK(r.after.flipped > 0);
    CHECK(r.anneal.cost == anneal_cost(p, r.anneal.pl, o.alpha_cross_mm, 0, nullptr, o.via_mm));
    CHECK(r.after.whpwl >= r.lb_any_rot);  // the any-rotation bound covers both sides of flippable parts
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
      const Part& pt = p.parts[i];
      if (!pt.may_flip()) CHECK_FALSE(flipped(pl.rot[i]));  // locked, fixed, through-hole, pinned (rule 6)
      if (!pt.movable) CHECK((pl.pos[i] == pt.pos0 && pl.rot[i] == 0));
      if (pt.ref == pinned) CHECK_FALSE(flipped(pl.rot[i]));
    }
    res.push_back(pl);
  }
  CHECK(res[0].pos == res[1].pos);
  CHECK(res[0].rot == res[1].rot);
}

TEST_CASE("flip: with no part allowed to flip, --flip changes nothing", "[place][flip]") {
  Problem p = random_problem(30, 5, 30 * MM);
  PlaceOptions o;
  o.runs = 2;
  o.threads = 2;
  o.effort = 0.3;
  Placement a = Placement::initial(p), b = Placement::initial(p);
  tmk::place::place(p, a, o);
  o.flip = true;
  tmk::place::place(p, b, o);
  CHECK(a.pos == b.pos);
  CHECK(a.rot == b.rot);
}

TEST_CASE("flip: swap states exchange absolute poses on either side", "[place][flip]") {
  Part a, b;
  a.angle0 = 0;
  b.angle0 = 90;  // dk = 1
  for (int sb = 0; sb < kStates; ++sb) {
    const int sa = swap_state(sb, 1);
    CHECK(flipped(sa) == flipped(sb));
    CHECK(std::fmod(a.angle_of(sa) - b.angle_of(sb) + 720.0, 360.0) == 0.0);
    const int back = swap_state(sa, -1);
    CHECK(std::fmod(b.angle_of(back) - a.angle_of(sa) + 720.0, 360.0) == 0.0);
  }
}
