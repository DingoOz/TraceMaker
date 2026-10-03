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
