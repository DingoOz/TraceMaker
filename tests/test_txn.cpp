// SPDX-License-Identifier: GPL-3.0-or-later
// Transactional negotiation (doc 16 §12, D99) on a small generated ball-grid breakout: the result is legal, does not depend on the
// thread count, and a route that rips others is kept only when their re-routes make up for it.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "drc/drc.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/legalize.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

// A 5 x 5 ball array (0.8 mm pitch, 0.36 mm pads, bottom layer) whose balls but the centre one each go to a pad on a ring around it, in a
// fixed shuffled order: many crossings on two layers, so negotiation has to rip and re-route.
std::string breakout_problem() {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -6.0}, {"maxX", 6.0}, {"minY", -6.0}, {"maxY", 6.0}};
  json obstacles = json::array(), connections = json::array();
  std::vector<std::pair<double, double>> ring;
  for (int i = 0; i < 8; ++i) ring.push_back({-4.2 + 1.2 * i, 5.2});
  for (int i = 0; i < 8; ++i) ring.push_back({-4.2 + 1.2 * i, -5.2});
  for (int i = 0; i < 4; ++i) ring.push_back({-5.2, -1.8 + 1.2 * i});
  for (int i = 0; i < 4; ++i) ring.push_back({5.2, -1.8 + 1.2 * i});
  int n = 0;
  for (int gy = -2; gy <= 2; ++gy)
    for (int gx = -2; gx <= 2; ++gx) {
      const double x = 0.8 * gx, y = 0.8 * gy;
      const bool ball_in_use = !(gx == 0 && gy == 0);
      const std::string id = "b" + std::to_string(n), tp = "t" + std::to_string(n);
      obstacles.push_back({{"type", "rect"}, {"layers", {"bottom"}}, {"center", {{"x", x}, {"y", y}}}, {"width", 0.36}, {"height", 0.36},
                           {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", "bga"}});
      if (ball_in_use) {
        const auto [tx, ty] = ring[static_cast<std::size_t>((n * 5) % 24)];
        obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", tx}, {"y", ty}}}, {"width", 0.6}, {"height", 0.5},
                             {"connectedTo", {tp}}, {"obstacleId", "pad_" + tp}, {"componentId", "T" + std::to_string(n)}});
        connections.push_back({{"name", "c" + std::to_string(n)},
                               {"pointsToConnect",
                                {{{"x", x}, {"y", y}, {"layer", "bottom"}, {"pointId", id}}, {{"x", tx}, {"y", ty}, {"layer", "top"}, {"pointId", tp}}}},
                               {"rootConnectionName", "c" + std::to_string(n)}});
      }
      ++n;
    }
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  return p.dump();
}

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

Loaded load_problem() {
  const auto files = io::srj_to_kicad(io::read_srj(breakout_problem()));
  const fs::path dir = fs::temp_directory_path() / ("tm_txn_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

int errors(model::Board b, const model::DesignRules& r, const route::RouteResult& res) {
  for (const auto& t : res.tracks) b.tracks.push_back(t);
  for (const auto& v : res.vias) b.vias.push_back(v);
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations)
    if (v.severity == "error" && v.type != "unconnected_items") ++n;
  return n;
}

route::RouterOptions options() {
  route::RouterOptions o;
  o.work_budget = 4'000'000;
  o.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  o.gpu_device = -1;
  o.txn = 1;
  o.txn_temp = 1.5;
  return o;
}

}  // namespace

TEST_CASE("transactional negotiation: legal result, identical at any thread count", "[route][txn]") {
  const auto l = load_problem();
  const auto one = route::route_portfolio(l.board, l.rules, options(), 2, {}, 1);
  const auto two = route::route_portfolio(l.board, l.rules, options(), 2, {}, 2);
  CHECK(one.best.routed == two.best.routed);
  CHECK(one.best.tracks.size() == two.best.tracks.size());
  CHECK(one.best.vias.size() == two.best.vias.size());
  CHECK(one.best.expansions == two.best.expansions);
  CHECK(one.best.txn_tried == two.best.txn_tried);
  CHECK(errors(l.board, l.rules, one.best) == 0);
  CHECK(one.best.routed > 0);
}

TEST_CASE("transactional negotiation: routes that rip others are tried and judged, plain negotiation tries none", "[route][txn]") {
  const auto l = load_problem();
  route::RouterOptions plain = options();
  plain.txn = 0;
  const auto p = route::route_portfolio(l.board, l.rules, plain, 1, {}, 1);
  const auto t = route::route_portfolio(l.board, l.rules, options(), 1, {}, 1);
  CHECK(p.best.txn_tried == 0);
  CHECK(t.best.txn_tried > 0);  // the problem needs rips: the transaction path runs
  CHECK(t.best.txn_kept <= t.best.txn_tried);
  CHECK(errors(l.board, l.rules, p.best) == 0);
  CHECK(errors(l.board, l.rules, t.best) == 0);
}

TEST_CASE("field cache: identical copper with and without it", "[route][txn]") {
  const auto l = load_problem();
  route::RouterOptions off = options();
  route::RouterOptions on = options();
  on.field_cache_mb = 256;
  const auto a = route::route_portfolio(l.board, l.rules, off, 1, {}, 1);
  const auto c = route::route_portfolio(l.board, l.rules, on, 1, {}, 1);
  REQUIRE(a.best.tracks.size() == c.best.tracks.size());
  REQUIRE(a.best.vias.size() == c.best.vias.size());
  for (std::size_t i = 0; i < a.best.tracks.size(); ++i) {
    CHECK(a.best.tracks[i].a == c.best.tracks[i].a);
    CHECK(a.best.tracks[i].b == c.best.tracks[i].b);
    CHECK(a.best.tracks[i].layer == c.best.tracks[i].layer);
  }
  CHECK(a.best.expansions == c.best.expansions);
}

TEST_CASE("saved state: written after each round, a resumed run starts from it and does not get worse", "[route][txn]") {
  const auto l = load_problem();
  const std::string path = (fs::temp_directory_path() / "tm_test_txn.state").string();
  fs::remove(path);
  route::RouterOptions first = options();
  first.round_work = 300000;
  first.rounds = 1;
  first.state_out = path;
  const auto a = route::route_portfolio(l.board, l.rules, first, 1, {}, 1);
  REQUIRE(fs::exists(path));
  route::RouterOptions again = first;
  again.state_in = path;
  const auto b = route::route_portfolio(l.board, l.rules, again, 1, {}, 1);
  CHECK(b.best.routed >= a.best.routed);
  fs::remove(path);
}

TEST_CASE("clearance ramp: loose stages first, the result is legal under the board's rules and the same at any thread count", "[route][txn]") {
  const auto l = load_problem();
  const double board_mm = nm_to_mm(std::max(l.rules.classes.front().clearance, l.rules.minimums.clearance));
  route::RouterOptions o = options();
  o.round_work = 300000;
  o.rounds = 8;
  o.ramp_from_mm = board_mm - 0.04;
  o.ramp_step_mm = 0.02;
  o.ramp_rounds = 1;
  const auto one = route::route_portfolio(l.board, l.rules, o, 2, {}, 1);
  const auto two = route::route_portfolio(l.board, l.rules, o, 2, {}, 2);
  CHECK(one.best.routed == two.best.routed);
  CHECK(one.best.tracks.size() == two.best.tracks.size());
  CHECK(one.best.vias.size() == two.best.vias.size());
  CHECK(one.best.routed > 0);
  CHECK(errors(l.board, l.rules, one.best) == 0);
}

TEST_CASE("clearance ramp with breathing: still legal under the board's rules and the same at any thread count", "[route][txn]") {
  const auto l = load_problem();
  const double board_mm = nm_to_mm(std::max(l.rules.classes.front().clearance, l.rules.minimums.clearance));
  route::RouterOptions o = options();
  o.round_work = 150000;
  o.rounds = 9;
  o.ramp_from_mm = board_mm - 0.04;
  o.ramp_step_mm = 0.02;
  o.ramp_rounds = 1;
  o.ramp_cycle = 1;
  o.ramp_stall = 1;
  const auto one = route::route_portfolio(l.board, l.rules, o, 2, {}, 1);
  const auto two = route::route_portfolio(l.board, l.rules, o, 2, {}, 2);
  CHECK(one.best.routed == two.best.routed);
  CHECK(one.best.tracks.size() == two.best.tracks.size());
  CHECK(one.best.routed > 0);
  CHECK(errors(l.board, l.rules, one.best) == 0);
  CHECK(errors(l.board, l.rules, two.best) == 0);
}

// Two long runs 0.06 mm apart (edge to edge) on a board that asks for 0.1 mm: legalisation moves their vertices apart, keeps the
// ends on their pads, leaves nothing for the exact DRC to flag, and is deterministic.
TEST_CASE("legalisation: parallel runs too close are moved apart, pad ends stay, the DRC is clean", "[route][legalize]") {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -2.0}, {"maxX", 12.0}, {"minY", -4.0}, {"maxY", 4.0}};
  json obstacles = json::array(), connections = json::array();
  const double ys[4] = {-2.0, -2.0, 2.0, 2.0}, xs[4] = {0.0, 10.0, 0.0, 10.0};
  for (int i = 0; i < 4; ++i) {
    const std::string id = "p" + std::to_string(i);
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", xs[i]}, {"y", ys[i]}}}, {"width", 0.5}, {"height", 0.5},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", "c"}});
  }
  for (int c = 0; c < 2; ++c)
    connections.push_back({{"name", "c" + std::to_string(c)},
                           {"pointsToConnect",
                            {{{"x", xs[2 * c]}, {"y", ys[2 * c]}, {"layer", "top"}, {"pointId", "p" + std::to_string(2 * c)}},
                             {{"x", xs[2 * c + 1]}, {"y", ys[2 * c + 1]}, {"layer", "top"}, {"pointId", "p" + std::to_string(2 * c + 1)}}}},
                           {"rootConnectionName", "c" + std::to_string(c)}});
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_leg_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "b.kicad_pcb") << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  const auto lb = io::read_board_file((dir / "b.kicad_pcb").string());
  const auto rules = io::read_design_rules((dir / "b.kicad_pcb").string());
  fs::remove_all(dir);
  const auto& b = lb.board;
  REQUIRE(b.pads.size() == 4);
  // Hand-made routes (the importer flips y: pads 0 and 1 are at y = +2): their net runs at y = +0.09 between x = 3 and 7, the other at
  // y = -0.09 (centres 0.18 apart, gap 0.06 < 0.1).
  auto track = [&](model::Point a, model::Point c, model::NetId net) {
    model::Track t;
    t.a = a;
    t.b = c;
    t.width = mm_to_nm(0.12);
    t.layer = 0;
    t.net = net;
    return t;
  };
  auto pt = [](double x, double y) { return model::Point{mm_to_nm(x), mm_to_nm(y)}; };
  std::vector<route::ConnRoute> routes(2);
  routes[0].tracks = {track(b.pads[0].pos, pt(3, 0.09), b.pads[0].net), track(pt(3, 0.09), pt(7, 0.09), b.pads[0].net),
                      track(pt(7, 0.09), b.pads[1].pos, b.pads[0].net)};
  routes[1].tracks = {track(b.pads[2].pos, pt(3, -0.09), b.pads[2].net), track(pt(3, -0.09), pt(7, -0.09), b.pads[2].net),
                      track(pt(7, -0.09), b.pads[3].pos, b.pads[2].net)};
  route::RouteResult before;
  for (const auto& cr : routes) before.tracks.insert(before.tracks.end(), cr.tracks.begin(), cr.tracks.end());
  CHECK(errors(b, rules, before) > 0);  // the clearance rule is broken before
  auto moved = routes;
  const auto r1 = route::legalize_routes(b, rules, moved);
  CHECK(r1.pairs_before > 0);
  CHECK(r1.pairs_after == 0);
  route::RouteResult after;
  for (const auto& cr : moved) after.tracks.insert(after.tracks.end(), cr.tracks.begin(), cr.tracks.end());
  CHECK(errors(b, rules, after) == 0);
  CHECK(moved[0].tracks.front().a == routes[0].tracks.front().a);  // the end on its pad stays
  CHECK(moved[1].tracks.back().b == routes[1].tracks.back().b);
  auto again = routes;
  const auto r2 = route::legalize_routes(b, rules, again);
  CHECK(r2.sweeps_used == r1.sweeps_used);
  for (std::size_t i = 0; i < moved.size(); ++i)
    for (std::size_t k = 0; k < moved[i].tracks.size(); ++k) {
      CHECK(moved[i].tracks[k].a == again[i].tracks[k].a);
      CHECK(moved[i].tracks[k].b == again[i].tracks[k].b);
    }
}

// A straight 10 mm track between two pads cannot move at all (both ends are fixed on their pads), so a locked neighbour that comes within
// 0.06 mm of its middle is a pinch the vertices of the routes cannot resolve. Cut into pieces of at most 1 mm the track bends away from it.
TEST_CASE("legalisation with subdivision: a straight track between fixed ends bends away from a pinch", "[route][legalize]") {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -2.0}, {"maxX", 12.0}, {"minY", -4.0}, {"maxY", 4.0}};
  json obstacles = json::array(), connections = json::array();
  const double ys[4] = {-2.0, -2.0, 2.0, 2.0}, xs[4] = {0.0, 10.0, 0.0, 10.0};
  for (int i = 0; i < 4; ++i) {
    const std::string id = "p" + std::to_string(i);
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", xs[i]}, {"y", ys[i]}}}, {"width", 0.5}, {"height", 0.5},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", "c"}});
  }
  for (int c = 0; c < 2; ++c)
    connections.push_back({{"name", "c" + std::to_string(c)},
                           {"pointsToConnect",
                            {{{"x", xs[2 * c]}, {"y", ys[2 * c]}, {"layer", "top"}, {"pointId", "p" + std::to_string(2 * c)}},
                             {{"x", xs[2 * c + 1]}, {"y", ys[2 * c + 1]}, {"layer", "top"}, {"pointId", "p" + std::to_string(2 * c + 1)}}}},
                           {"rootConnectionName", "c" + std::to_string(c)}});
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_leg_sub_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "b.kicad_pcb") << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  const auto lb = io::read_board_file((dir / "b.kicad_pcb").string());
  const auto rules = io::read_design_rules((dir / "b.kicad_pcb").string());
  fs::remove_all(dir);
  const auto& b = lb.board;
  REQUIRE(b.pads.size() == 4);
  auto track = [&](model::Point a, model::Point c, model::NetId net, bool locked) {
    model::Track t;
    t.a = a;
    t.b = c;
    t.width = mm_to_nm(0.12);
    t.layer = 0;
    t.net = net;
    t.locked = locked;
    return t;
  };
  auto pt = [](double x, double y) { return model::Point{mm_to_nm(x), mm_to_nm(y)}; };
  // Net of pads 0 and 1: one straight track pad to pad at y = +2. The other net, locked, comes up to y = +1.82 at x = 5 (centres 0.18 apart,
  // gap 0.06 < 0.1) and goes down again.
  std::vector<route::ConnRoute> routes(2);
  routes[0].tracks = {track(b.pads[0].pos, b.pads[1].pos, b.pads[0].net, false)};
  routes[1].tracks = {track(b.pads[2].pos, pt(3, -2), b.pads[2].net, true), track(pt(3, -2), pt(5, 1.82), b.pads[2].net, true),
                      track(pt(5, 1.82), pt(7, -2), b.pads[2].net, true), track(pt(7, -2), b.pads[3].pos, b.pads[2].net, true)};
  auto plain = routes;
  route::legalize_routes(b, rules, plain);  // nothing in the pinch can move: no vertex of the routes is free to resolve it
  CHECK(plain[0].tracks.size() == 1);
  route::RouteResult unmoved;
  for (const auto& cr : plain) unmoved.tracks.insert(unmoved.tracks.end(), cr.tracks.begin(), cr.tracks.end());
  CHECK(errors(b, rules, unmoved) > 0);
  route::LegalizeOptions lo;
  lo.subdivide_mm = 1.0;
  auto cut = routes;
  const auto r1 = route::legalize_routes(b, rules, cut, lo);
  CHECK(r1.pairs_before > 0);
  CHECK(r1.pairs_after == 0);
  CHECK(cut[0].tracks.size() > 1);  // it bent
  CHECK(cut[0].tracks.size() < 12);  // and the pieces that stayed straight were merged again
  CHECK(cut[0].tracks.front().a == routes[0].tracks.front().a);  // the ends stay on their pads
  CHECK(cut[0].tracks.back().b == routes[0].tracks.back().b);
  for (std::size_t k = 0; k + 1 < cut[0].tracks.size(); ++k) CHECK(cut[0].tracks[k].b == cut[0].tracks[k + 1].a);  // one connected chain
  route::RouteResult after;
  for (const auto& cr : cut) after.tracks.insert(after.tracks.end(), cr.tracks.begin(), cr.tracks.end());
  CHECK(errors(b, rules, after) == 0);
  auto again = routes;
  const auto r2 = route::legalize_routes(b, rules, again, lo);
  CHECK(r2.sweeps_used == r1.sweeps_used);
  REQUIRE(again[0].tracks.size() == cut[0].tracks.size());
  for (std::size_t k = 0; k < cut[0].tracks.size(); ++k) CHECK(cut[0].tracks[k].a == again[0].tracks[k].a);
}

TEST_CASE("relax late: plain rounds first, stages afresh when they stall; legal and the same at any thread count", "[route][txn]") {
  const auto l = load_problem();
  route::RouterOptions o = options();
  o.relax = true;
  o.relax_late = true;
  o.round_work = 60000;  // too little to finish: the plain rounds stall and the stages start from nothing
  o.rounds = 10;
  o.ramp_rounds = 1;
  o.ramp_stall = 1;
  o.ramp_cycle = 1;
  const auto one = route::route_portfolio(l.board, l.rules, o, 2, {}, 1);
  const auto two = route::route_portfolio(l.board, l.rules, o, 2, {}, 2);
  CHECK(one.best.routed == two.best.routed);
  CHECK(one.best.tracks.size() == two.best.tracks.size());
  CHECK(one.best.routed > 0);
  CHECK(errors(l.board, l.rules, one.best) == 0);
}
