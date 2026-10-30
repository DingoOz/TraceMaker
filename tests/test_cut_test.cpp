// Cut test of a routed topology (route/cut_test.hpp, doc 05 §67): a ball-grid cell of four 0.36 mm pads at 0.8 mm pitch, rules 0.12 mm
// tracks, 0.1 mm clearance, 0.3 mm vias. A via alone fits in the cell; a via with a track squeezed between it and a pad does not, wherever
// it goes in the cell (diagonal 0.622 mm < 0.3 + 0.12 + 3 x 0.1); two tracks do not fit between two pads (0.44 mm < 2 x 0.12 + 3 x 0.1).
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/cut_test.hpp"

using namespace tmk;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Cell {
  model::Board b;
  model::DesignRules rules;
  model::Point p0, centre;
  int ux = 1, uy = 1;  // direction from pad 0 towards the cell's centre
  model::NetId net_wire = 0, net_wire2 = 0, net_via = 0;
};

Cell make_cell() {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -3.0}, {"maxX", 9.0}, {"minY", -4.0}, {"maxY", 4.0}};
  json obstacles = json::array(), connections = json::array();
  // Pads 0-3 are the cell; each connection joins one of them (or nothing of the cell) to a pad far away.
  const double xs[14] = {0, 0.8, 0, 0.8, 6, 6, 6, 6, 6, 7, 6, 7, 6, 7}, ys[14] = {0, 0, 0.8, 0.8, -3, -2, -1, 0, 2, 2, 3, 3, 1, 1};
  for (int i = 0; i < 14; ++i) {
    const std::string id = "p" + std::to_string(i);
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", xs[i]}, {"y", ys[i]}}}, {"width", 0.36}, {"height", 0.36},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", "c"}});
  }
  const int pairs[7][2] = {{0, 4}, {1, 5}, {2, 6}, {3, 7}, {8, 9}, {10, 11}, {12, 13}};
  for (int c = 0; c < 7; ++c)
    connections.push_back({{"name", "c" + std::to_string(c)},
                           {"pointsToConnect",
                            {{{"x", xs[pairs[c][0]]}, {"y", ys[pairs[c][0]]}, {"layer", "top"}, {"pointId", "p" + std::to_string(pairs[c][0])}},
                             {{"x", xs[pairs[c][1]]}, {"y", ys[pairs[c][1]]}, {"layer", "top"}, {"pointId", "p" + std::to_string(pairs[c][1])}}}},
                           {"rootConnectionName", "c" + std::to_string(c)}});
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_cut_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "b.kicad_pcb") << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Cell c;
  c.b = io::read_board_file((dir / "b.kicad_pcb").string()).board;
  c.rules = io::read_design_rules((dir / "b.kicad_pcb").string());
  fs::remove_all(dir);
  for (auto& cl : c.rules.classes) cl.clearance = mm_to_nm(0.1), cl.track_width = mm_to_nm(0.12);
  c.rules.minimums.clearance = mm_to_nm(0.1);
  REQUIRE(c.b.pads.size() == 14);
  c.p0 = c.b.pads[0].pos;
  model::Point s{0, 0};
  for (int i = 0; i < 4; ++i) s.x += c.b.pads[static_cast<std::size_t>(i)].pos.x / 4, s.y += c.b.pads[static_cast<std::size_t>(i)].pos.y / 4;
  c.centre = s;
  c.ux = c.centre.x > c.p0.x ? 1 : -1;
  c.uy = c.centre.y > c.p0.y ? 1 : -1;
  c.net_wire = c.b.pads[8].net;
  c.net_via = c.b.pads[10].net;
  c.net_wire2 = c.b.pads[12].net;
  return c;
}

model::Point rel(const Cell& c, double dx, double dy) {
  return {c.p0.x + c.ux * mm_to_nm(dx), c.p0.y + c.uy * mm_to_nm(dy)};
}

void add_track(Cell& c, model::Point a, model::Point b, model::NetId net) {
  model::Track t;
  t.a = a;
  t.b = b;
  t.width = mm_to_nm(0.12);
  t.layer = 0;
  t.net = net;
  c.b.tracks.push_back(t);
}

void add_via(Cell& c, model::Point at, model::NetId net) {
  model::Via v;
  v.pos = at;
  v.size = mm_to_nm(0.3);
  v.drill = mm_to_nm(0.15);
  v.layer_top = 0;
  v.layer_bottom = c.b.copper_count() - 1;
  v.net = net;
  c.b.vias.push_back(v);
}

}  // namespace

TEST_CASE("cut test: a via alone fits in a ball-grid cell", "[route][cuts]") {
  Cell c = make_cell();
  add_via(c, c.centre, c.net_via);
  const auto r = route::cut_test(c.b, c.rules);
  CHECK(r.cuts > 0);
  CHECK(r.overfull.empty());
  CHECK(r.vias_without_room.empty());
  CHECK(r.s_star >= 1.0);
}

TEST_CASE("cut test: a via with a track between it and a pad has no room anywhere in the cell", "[route][cuts]") {
  Cell c = make_cell();
  add_via(c, c.centre, c.net_via);
  // The track enters the cell between pads 0 and 1, bends between pad 0 and the via, and leaves between pads 0 and 2.
  const model::Point bend = rel(c, 0.24, 0.26);
  add_track(c, rel(c, 0.40, -1.0), bend, c.net_wire);
  add_track(c, bend, rel(c, -1.0, 0.40), c.net_wire);
  const auto r = route::cut_test(c.b, c.rules);
  bool via_cut = false;
  for (const auto& o : r.overfull) via_cut |= (o.a.via || o.b.via) && o.wires.size() == 1 && o.wires[0] == c.net_wire;
  CHECK(via_cut);
  REQUIRE(r.vias_without_room.size() == 1);
  CHECK(r.vias_without_room[0] == 0);
  // Without the via the same track has room: the pad gaps take one track each.
  Cell d = make_cell();
  add_track(d, rel(d, 0.40, -1.0), rel(d, 0.24, 0.26), d.net_wire);
  add_track(d, rel(d, 0.24, 0.26), rel(d, -1.0, 0.40), d.net_wire);
  const auto r2 = route::cut_test(d.b, d.rules);
  CHECK(r2.overfull.empty());
}

TEST_CASE("cut test: two tracks between two pads 0.44 mm apart is over-full; a loop that encloses nothing does not count", "[route][cuts]") {
  Cell c = make_cell();
  add_track(c, rel(c, 0.33, -1.0), rel(c, 0.33, 0.4), c.net_wire);
  add_track(c, rel(c, 0.47, -1.0), rel(c, 0.47, 0.4), c.net_wire2);
  const auto r = route::cut_test(c.b, c.rules);
  bool two = false;
  for (const auto& o : r.overfull)
    if (!o.a.via && !o.b.via && o.wires.size() == 2) {
      two = true;
      REQUIRE(o.wire_tracks.size() == 2);  // the board tracks they cross on, for pruning
      CHECK(((o.wire_tracks[0] == 0 && o.wire_tracks[1] == 1) || (o.wire_tracks[0] == 1 && o.wire_tracks[1] == 0)));
    }
  CHECK(two);
  CHECK(r.s_star < 1.0);
  // One track that dips into the gap between pads 0 and 1 and comes back out on the same side crosses the cut twice; the loop it makes
  // with the cut holds no pad or via, so it can be pulled out, and the other track alone has room.
  Cell d = make_cell();
  add_track(d, rel(d, 0.33, -1.0), rel(d, 0.33, 0.4), d.net_wire);
  add_track(d, rel(d, 0.47, -1.0), rel(d, 0.47, 0.05), d.net_wire2);
  add_track(d, rel(d, 0.47, 0.05), rel(d, 0.55, 0.05), d.net_wire2);
  add_track(d, rel(d, 0.55, 0.05), rel(d, 0.55, -1.0), d.net_wire2);
  const auto r2 = route::cut_test(d.b, d.rules);
  CHECK(r2.crossings_cancelled >= 2);
  bool any_two = false;
  for (const auto& o : r2.overfull) any_two |= o.wires.size() == 2;
  CHECK_FALSE(any_two);
}
