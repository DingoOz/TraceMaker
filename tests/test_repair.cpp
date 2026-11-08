// SPDX-License-Identifier: GPL-3.0-or-later
// Repair windows (issue #56, doc 05 §73, D172): a pin walled in by another net's unlocked copper is opened by lifting that copper and
// routing both again; locked copper is never lifted; the result is deterministic and has no DRC errors.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "drc/drc.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "passes/repair.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

// Pad a1 at the origin goes to a2, 5 mm to the right. Net b's two pads sit 2 mm to the left of a1, 0.3 mm apart (too close for a track
// between them), and a track of net b runs from one round a1 to the other: a1 is walled in on the top layer, and with no vias it
// cannot get out. Routed the other way (b straight across its gap), both connect.
std::string problem() {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.15;
  p["bounds"] = {{"minX", -5.0}, {"maxX", 8.0}, {"minY", -4.0}, {"maxY", 4.0}};
  auto pad = [](const std::string& id, double x, double y, double w, double h) {
    return json{{"type", "rect"},        {"layers", {"top"}},     {"center", {{"x", x}, {"y", y}}}, {"width", w}, {"height", h},
                {"connectedTo", {id}},   {"obstacleId", "pad_" + id}, {"componentId", "U_" + id}};
  };
  p["obstacles"] = {pad("a1", 0, 0, 0.6, 0.6), pad("a2", 5, 0, 0.6, 0.6), pad("b1", -2, 0.45, 0.8, 0.6), pad("b2", -2, -0.45, 0.8, 0.6)};
  auto conn = [](const std::string& n, const std::string& x, double xx, double xy, const std::string& y, double yx, double yy) {
    return json{{"name", n},
                {"pointsToConnect", {{{"x", xx}, {"y", xy}, {"layer", "top"}, {"pointId", x}}, {{"x", yx}, {"y", yy}, {"layer", "top"}, {"pointId", y}}}},
                {"rootConnectionName", n}};
  };
  p["connections"] = {conn("a", "a1", 0, 0, "a2", 5, 0), conn("b", "b1", -2, 0.45, "b2", -2, -0.45)};
  return p.dump();
}

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

// The problem with net b's ring already on the board (as a routed board's copper), locked or not.
Loaded load(bool locked) {
  const auto files = io::srj_to_kicad(io::read_srj(problem()));
  const fs::path dir = fs::temp_directory_path() / ("tm_repair_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  // The converter may move the origin: the ring is placed relative to the pads as read (b1 and b2 are the two leftmost, a1 the next).
  std::vector<const model::Pad*> pads;
  for (const auto& pd : l.board.pads) pads.push_back(&pd);
  REQUIRE(pads.size() == 4);
  std::sort(pads.begin(), pads.end(), [](const model::Pad* x, const model::Pad* y) { return x->pos.x != y->pos.x ? x->pos.x < y->pos.x : x->pos.y < y->pos.y; });
  const model::NetId nb = pads[0]->net;
  REQUIRE(nb > 0);
  REQUIRE(pads[1]->net == nb);
  const Coord bx = pads[0]->pos.x, lo = pads[0]->pos.y, hi = pads[1]->pos.y, ox = pads[2]->pos.x, oy = pads[2]->pos.y;
  const Coord d = 2'000'000;
  const std::vector<model::Point> ring{{bx, lo}, {bx, oy - d}, {ox + d, oy - d}, {ox + d, oy + d}, {bx, oy + d}, {bx, hi}};
  for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
    model::Track t;
    t.a = ring[i];
    t.b = ring[i + 1];
    t.width = 150'000;
    t.layer = 0;
    t.net = nb;
    t.locked = locked;
    l.board.tracks.push_back(t);
  }
  return l;
}

route::RouterOptions options() {
  route::RouterOptions o;
  o.work_budget = 500'000;
  o.time_limit_s = 3600;  // the work budget decides
  o.gpu_device = -1;
  o.allow_vias = false;
  return o;
}

int errors(const model::Board& base, const model::DesignRules& r, const passes::RepairResult& rep) {
  model::Board b = base;
  for (auto it = rep.ripped_tracks.rbegin(); it != rep.ripped_tracks.rend(); ++it) b.tracks.erase(b.tracks.begin() + *it);
  for (auto it = rep.ripped_vias.rbegin(); it != rep.ripped_vias.rend(); ++it) b.vias.erase(b.vias.begin() + *it);
  b.tracks.insert(b.tracks.end(), rep.tracks.begin(), rep.tracks.end());
  b.vias.insert(b.vias.end(), rep.vias.begin(), rep.vias.end());
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations)
    if (v.severity == "error") ++n;  // unconnected items included: everything must be joined
  return n;
}

}  // namespace

TEST_CASE("repair windows: a pin walled in by unlocked copper is opened, and both nets end joined", "[repair]") {
  const auto l = load(false);
  const auto plain = route::Router(l.board, l.rules, options()).run();
  REQUIRE(plain.connections == 1);  // net b is joined by its ring
  REQUIRE(plain.routed == 0);       // a1 is walled in
  passes::RepairOptions ro;
  const auto rep = passes::repair_open(l.board, l.rules, plain, options(), ro);
  CHECK(rep.open_before == 1);
  CHECK(rep.open_after == 0);
  CHECK(rep.changed);
  CHECK(rep.ripped_tracks.size() == 5);  // the whole branch between b1 and b2, not part of it
  CHECK(errors(l.board, l.rules, rep) == 0);
  // The 1 mm window does not reach the ring (2 mm from a1): the first attempt that lifts something is the 2 mm one.
  REQUIRE(!rep.steps.empty());
  CHECK(rep.steps.front().radius_mm == 2);
  CHECK(rep.steps.back().kept);
  CHECK(passes::open_connections(l.board, l.rules) == 1);

  SECTION("deterministic") {
    const auto again = passes::repair_open(l.board, l.rules, plain, options(), ro);
    REQUIRE(again.tracks.size() == rep.tracks.size());
    for (std::size_t i = 0; i < rep.tracks.size(); ++i) {
      CHECK(again.tracks[i].a == rep.tracks[i].a);
      CHECK(again.tracks[i].b == rep.tracks[i].b);
    }
    CHECK(again.ripped_tracks == rep.ripped_tracks);
    CHECK(again.work == rep.work);
  }
  SECTION("a window that would lift more branches than allowed is not tried") {
    passes::RepairOptions small = ro;
    small.max_branches = 0;
    const auto none = passes::repair_open(l.board, l.rules, plain, options(), small);
    CHECK(!none.changed);
    CHECK(none.steps.empty());
    CHECK(none.open_after == 1);
  }
}

TEST_CASE("repair windows: locked copper is never lifted", "[repair]") {
  const auto l = load(true);
  const auto plain = route::Router(l.board, l.rules, options()).run();
  REQUIRE(plain.routed == 0);
  const auto rep = passes::repair_open(l.board, l.rules, plain, options(), passes::RepairOptions{});
  CHECK(!rep.changed);
  CHECK(rep.ripped_tracks.empty());
  CHECK(rep.steps.empty());  // nothing in any window may be lifted
  CHECK(rep.open_after == 1);
}
