// Unit tests for the placer (tm::place).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "core/rng.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "place/anneal.hpp"
#include "place/global.hpp"
#include "place/legalize.hpp"
#include "place/lower_bound.hpp"
#include "place/placer.hpp"
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
    }
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

TEST_CASE("translated closer() agrees with geom::closer_than", "[place]") {
  const RngStream rng(5, 2, 0);
  std::uint64_t k = 0;
  auto u = [&] { return rng.uniform(k++); };
  for (int it = 0; it < 3000; ++it) {
    std::vector<Point> a, b;
    for (int i = 0; i < 4; ++i) a.push_back({static_cast<Coord>(u() * 4e6), static_cast<Coord>(u() * 4e6)});
    a = convex_hull(a);
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
