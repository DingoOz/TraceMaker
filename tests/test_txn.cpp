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
