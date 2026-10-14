// SPDX-License-Identifier: GPL-3.0-or-later
// Stall diagnostics (issue #8, doc 09 section 8, doc 13): a failed connection's `failure` event carries the width and clearance of the
// attempt, how much room the passage that closed had, a verdict (placement, rule, contested, unknown), the path that was tried and the
// rule it came from. Measuring them never changes the routing: a run with a sink attached gives the same copper as one without.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

struct Capture final : events::Sink {
  std::vector<json> failures;
  void publish(std::string text) override {
    const auto j = json::parse(text, nullptr, false);
    if (!j.is_discarded() && j.value("type", "") == "failure") failures.push_back(j);
  }
  bool wants_transient() const override { return false; }
};

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

// One copper layer, 20 x 20 mm. A wall of fixed copper (two bars with a gap of `gap` mm between them) crosses the board at x = 0; net A
// runs from (-8, 0) to (8, 0), through the gap, and has no other way. The rules: net class width, clearance, board minimum width.
Loaded wall_board(double gap, double width, double clearance, double min_width, bool second_net = false) {
  json p;
  p["layerCount"] = 1;
  p["minTraceWidth"] = min_width;
  p["nominalTraceWidth"] = width;
  p["defaultObstacleMargin"] = clearance;
  p["bounds"] = {{"minX", -10.0}, {"maxX", 10.0}, {"minY", -10.0}, {"maxY", 10.0}};
  json obstacles = json::array(), connections = json::array();
  auto pad = [&](const std::string& id, const std::string& part, double x, double y) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", 0.6}, {"height", 0.5},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", part}});
  };
  auto bar = [&](const std::string& id, double cy, double h) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", 0.0}, {"y", cy}}}, {"width", 0.6}, {"height", h},
                         {"obstacleId", id}, {"componentId", "WALL"}});
  };
  const double ax = second_net ? 9.7 : 8.0;  // with a second net, A spans the board too, so the two must cross
  pad("a1", "U1", -ax, 0);
  pad("a2", "U2", ax, 0);
  connections.push_back({{"name", "NET_A"},
                         {"pointsToConnect", {{{"x", -ax}, {"y", 0.0}, {"layer", "top"}, {"pointId", "a1"}}, {{"x", ax}, {"y", 0.0}, {"layer", "top"}, {"pointId", "a2"}}}},
                         {"rootConnectionName", "NET_A"}});
  if (gap > 0) {  // the wall: up from -10 to -gap/2, and from gap/2 to 10
    const double h = 10 - gap / 2;
    bar("wall_lo", -(gap / 2 + h / 2), h);
    bar("wall_hi", gap / 2 + h / 2, h);
  }
  if (second_net) {  // net B across the whole board at x = 0, no gap in it for A: B's copper is what closes A's way
    pad("b1", "U3", 0, -9.7);  // the pads end 0.05 mm short of the board edge: no way around them
    pad("b2", "U4", 0, 9.7);
    connections.push_back({{"name", "NET_B"},
                           {"pointsToConnect", {{{"x", 0.0}, {"y", -9.7}, {"layer", "top"}, {"pointId", "b1"}}, {{"x", 0.0}, {"y", 9.7}, {"layer", "top"}, {"pointId", "b2"}}}},
                           {"rootConnectionName", "NET_B"}});
  }
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_stall_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

route::RouterOptions options(events::Sink* sink) {
  route::RouterOptions o;
  o.work_budget = 3'000'000;
  o.gpu_device = -1;
  o.optimize = false;
  o.sink = sink;
  return o;
}

}  // namespace

TEST_CASE("stall: a gap too narrow for any legal track is a placement problem", "[stall][route]") {
  // Class width 0.25, clearance 0.2: a 0.3 mm gap leaves no room for a track of any width (0.3 - 2 x 0.2 < 0).
  const auto l = wall_board(0.3, 0.25, 0.2, 0.1);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 0);
  REQUIRE_FALSE(cap.failures.empty());
  const auto& f = cap.failures.back();
  INFO(f.dump());
  CHECK(f["width_nm"] == 250'000);
  CHECK(f["clearance_nm"] == 200'000);
  CHECK(f["min_width_nm"] == 100'000);
  CHECK(f["verdict"] == "placement");
  CHECK(f["gap_nm"] == 0);
  CHECK(f["gap_layer"] == 0);
  CHECK(f["reason"] == "boxed_in");  // the wall closes the source side: the open list ran out inside the pocket
  CHECK(f["rule"].get<std::string>().find("netclass") == 0);
  // No way gets through, so the place to look at is the closest approach: in front of the wall, on the line to the target.
  REQUIRE(f.contains("at"));
  CHECK(f["at"][0].get<long long>() > -1'000'000);
  CHECK(f["at"][0].get<long long>() <= 0);
  CHECK(std::llabs(f["at"][1].get<long long>()) <= 200'000);
  // The path that was tried starts at the source pad and stops there.
  REQUIRE(f["rejected"].size() >= 2);
  CHECK(f["rejected"].front()[0].get<long long>() < -7'000'000);
  CHECK(f["rejected"].back() == json::array({f["at"][0], f["at"][1], 0}));
  // The older fields are still there (old readers ignore the new ones).
  CHECK(f["type"] == "failure");
  CHECK(f.contains("a"));
  CHECK(f.contains("region"));
}

TEST_CASE("stall: a gap a narrower legal track passes is a rule problem", "[stall][route]") {
  // Clearance 0.1: a 0.32 mm gap takes a track of 0.12 mm. The class asks for 0.25; the board minimum is 0.1 (the router's own neck-down
  // stops at 0.15), so a narrower class width, not a moved part, opens the way.
  const auto l = wall_board(0.32, 0.25, 0.1, 0.1);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 0);
  REQUIRE_FALSE(cap.failures.empty());
  const auto& f = cap.failures.back();
  INFO(f.dump());
  CHECK(f["verdict"] == "rule");
  CHECK(f["gap_nm"].get<long long>() >= 100'000);
  CHECK(f["gap_nm"].get<long long>() < 250'000);
  CHECK(f["rule"].get<std::string>().find("fits") != std::string::npos);
}

TEST_CASE("stall: routed copper of another net closing the way is contested", "[stall][route]") {
  // A free board: one layer, no wall, but net B runs across the whole board at x = 0, and A has no layer to change to.
  const auto l = wall_board(0, 0.25, 0.1, 0.1, true);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 1);  // one of the two nets routes; the other is closed out by it
  REQUIRE_FALSE(cap.failures.empty());
  bool contested = false;
  for (const auto& f : cap.failures) contested = contested || f["verdict"] == "contested";
  INFO(cap.failures.back().dump());
  CHECK(contested);
  const json* c = nullptr;
  for (const auto& f : cap.failures)
    if (f["verdict"] == "contested") c = &f;
  REQUIRE(c != nullptr);
  CHECK((*c)["gap_fixed_nm"].get<long long>() >= 250'000);  // fixed copper alone leaves room for the class width ...
  CHECK((*c)["gap_nm"].get<long long>() < 250'000);         // ... the routed copper of the other net does not
}

TEST_CASE("stall: a viewer changes nothing about the routing", "[stall][route]") {
  for (const auto& l : {wall_board(0.3, 0.25, 0.2, 0.1), wall_board(0, 0.25, 0.1, 0.1, true), wall_board(2.0, 0.25, 0.1, 0.1)}) {
    Capture cap;
    const auto with = route::Router(l.board, l.rules, options(&cap)).run();
    const auto without = route::Router(l.board, l.rules, options(nullptr)).run();
    REQUIRE(with.tracks.size() == without.tracks.size());
    REQUIRE(with.vias.size() == without.vias.size());
    for (std::size_t i = 0; i < with.tracks.size(); ++i) {
      CHECK(with.tracks[i].a == without.tracks[i].a);
      CHECK(with.tracks[i].b == without.tracks[i].b);
      CHECK(with.tracks[i].layer == without.tracks[i].layer);
    }
    CHECK(with.routed == without.routed);
    CHECK(with.expansions == without.expansions);  // not one expansion more: the probe is bookkeeping, not search
  }
}

TEST_CASE("stall: a connection that routes has no failure to explain", "[stall][route]") {
  const auto l = wall_board(2.0, 0.25, 0.1, 0.1);  // a 2 mm gap: plenty
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  CHECK(res.routed == 1);
  CHECK(cap.failures.empty());
}
