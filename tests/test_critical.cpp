// SPDX-License-Identifier: GPL-3.0-or-later
// Critical nets (issue #24, doc 05 section 44): the three rules JLCPCB gives for routing, each with a test that shows it working and one that shows
// it off when it is not asked for. The boards are built here: an SRJ problem for the pads and rules, a zone with a split in it written on top.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/region.hpp"
#include "analysis/return_path.hpp"
#include "crules/critical.hpp"
#include "crules/detect.hpp"
#include "geom/clip.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/ref_map.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using namespace tmk;
using nlohmann::json;

namespace {

constexpr Coord MM = 1'000'000;

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

// Two-layer board, 40 x 40 mm. `pads` are (name, net, x, y, layer); every net connects its pads in order. `zone_rings`: filled rings of a ground plane
// on B.Cu (mm), written as the zone's fill.
struct Pad {
  std::string net;
  double x, y;
  const char* layer;
};

Loaded make_board(const std::vector<Pad>& pads, const std::vector<std::vector<std::pair<double, double>>>& zone_rings, const std::vector<std::array<double, 5>>& bars = {},
                  double outline_half = 20) {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.15;
  p["nominalTraceWidth"] = 0.2;
  p["defaultObstacleMargin"] = 0.15;
  p["bounds"] = {{"minX", -20.0}, {"maxX", 20.0}, {"minY", -20.0}, {"maxY", 20.0}};
  json obstacles = json::array(), connections = json::array();
  std::map<std::string, json> points;
  std::vector<std::string> order;
  int k = 0;
  for (const auto& pd : pads) {
    const std::string id = "p" + std::to_string(k++);
    obstacles.push_back({{"type", "rect"}, {"layers", {pd.layer}}, {"center", {{"x", pd.x}, {"y", pd.y}}}, {"width", 0.6}, {"height", 0.5}, {"connectedTo", {id}},
                         {"obstacleId", "pad_" + id}, {"componentId", "U" + std::to_string(k)}});
    if (!points.count(pd.net)) {
      points[pd.net] = json::array();
      order.push_back(pd.net);
    }
    points[pd.net].push_back({{"x", pd.x}, {"y", pd.y}, {"layer", pd.layer}, {"pointId", id}});
  }
  for (const auto& n : order) connections.push_back({{"name", n}, {"pointsToConnect", points[n]}, {"rootConnectionName", n}});
  for (const auto& b : bars)  // x, y of the centre, width, height, layer (0 top): fixed copper of no net
    obstacles.push_back({{"type", "rect"}, {"layers", {b[4] == 0 ? "top" : "bottom"}}, {"center", {{"x", b[0]}, {"y", b[1]}}}, {"width", b[2]}, {"height", b[3]},
                         {"obstacleId", "bar" + std::to_string(k++)}, {"componentId", "WALL"}});
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  std::string pcb = files.pcb;
  if (!zone_rings.empty()) {
    std::ostringstream z;
    z << "(zone (net \"GND\") (net_name \"GND\") (layer \"B.Cu\") (uuid \"6f0c3b1a-0000-4000-8000-000000000001\") (hatch edge 0.5) (connect_pads (clearance 0.2)) "
         "(min_thickness 0.2) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts";
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{{-outline_half, -outline_half}, {outline_half, -outline_half}, {outline_half, outline_half}, {-outline_half, outline_half}})
      z << " (xy " << x << " " << y << ")";
    z << "))\n";
    for (const auto& ring : zone_rings) {
      z << "(filled_polygon (layer \"B.Cu\") (pts";
      for (const auto& [x, y] : ring) z << " (xy " << x << " " << y << ")";
      z << "))\n";
    }
    z << ")\n";
    pcb.insert(pcb.rfind(')'), z.str());
  }
  const fs::path dir = fs::temp_directory_path() / ("tm_critical_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path f = dir / "b.kicad_pcb";
  std::ofstream(f) << pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(f.string()).board, io::read_design_rules(f.string())};
  fs::remove_all(dir);
  return l;
}

using Ring = std::vector<std::pair<double, double>>;
// Ground plane 30 x 30 mm on B.Cu. `bridge`: one piece with a slot (x -1..1) open to the top and joined along the bottom (y -15..-8); otherwise two halves.
std::vector<Ring> plane(bool bridge) {
  if (bridge) return {{{-15, -15}, {15, -15}, {15, 15}, {1, 15}, {1, -8}, {-1, -8}, {-1, 15}, {-15, 15}}};
  return {{{-15, -15}, {-1, -15}, {-1, 15}, {-15, 15}}, {{1, -15}, {15, -15}, {15, 15}, {1, 15}}};
}

// The GND net needs pads of its own to exist: two in the corner, off the signal's way.
std::vector<Pad> with_ground(std::vector<Pad> pads) {
  pads.push_back({"GND", -18, 18, "top"});
  pads.push_back({"GND", -16, 18, "top"});
  return pads;
}

route::RouterOptions options() {
  route::RouterOptions o;
  o.work_budget = 3'000'000;
  o.gpu_device = -1;
  o.optimize = false;
  o.seed = 1;
  return o;
}

route::RouteResult run(const Loaded& l, const route::RouterOptions& o) { return route::Router(l.board, l.rules, o).run(); }

double length_of(const route::RouteResult& r, const model::Board& b, const std::string& net) {
  double len = 0;
  for (const auto& t : r.tracks)
    if (b.nets[static_cast<std::size_t>(t.net)].name == net) len += std::hypot(static_cast<double>(t.a.x - t.b.x), static_cast<double>(t.a.y - t.b.y)) / 1e6;
  return len;
}

int vias_of(const route::RouteResult& r, const model::Board& b, const std::string& net) {
  return static_cast<int>(std::count_if(r.vias.begin(), r.vias.end(), [&](const model::Via& v) { return b.nets[static_cast<std::size_t>(v.net)].name == net; }));
}

// The audit of `net` on the board as routed: voids of its reference plane under it, and splits.
analysis::NetReturn audit(const Loaded& l, const route::RouteResult& r, const std::string& net) {
  model::Board routed = l.board;
  routed.tracks.insert(routed.tracks.end(), r.tracks.begin(), r.tracks.end());
  routed.vias.insert(routed.vias.end(), r.vias.begin(), r.vias.end());
  analysis::ReturnPathOptions ro;
  ro.name_patterns = {net};
  ro.refill = false;  // the fills written in the test are the plane
  const auto rep = analysis::analyse_return_paths(routed, l.rules, ro);
  REQUIRE(rep.nets.size() == 1);
  return rep.nets[0];
}

}  // namespace

TEST_CASE("critical: the rasterised plane equals a scan of the region, cell by cell", "[critical][index]") {
  // A plane with round antipads, a slot and an island: rasterised on a lattice and compared with the linear point-in-region reference (rule 3).
  std::mt19937_64 rng(11);
  auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
  geom::Rings holes;
  for (int i = 0; i < 40; ++i) holes.push_back(geom::circle_ring({static_cast<Coord>(uni(2, 38) * MM), static_cast<Coord>(uni(2, 28) * MM)}, static_cast<Coord>(uni(0.2, 0.9) * MM), 5'000));
  holes.push_back({{18 * MM, -1 * MM}, {19 * MM, -1 * MM}, {19 * MM, 31 * MM}, {18 * MM, 31 * MM}});  // a slot
  geom::Rings region = geom::subtract({{{0, 0}, {40 * MM, 0}, {40 * MM, 30 * MM}, {0, 30 * MM}}}, holes);
  region.push_back({{5 * MM, 5 * MM}, {5 * MM, 7 * MM}, {7 * MM, 7 * MM}, {7 * MM, 5 * MM}});
  region = geom::unite(region);
  const analysis::detail::Region reference(region);
  const geom::Box lat{-1 * MM, -1 * MM, 41 * MM, 31 * MM};
  for (const Coord pitch : {Coord{50'000}, Coord{73'000}}) {
    const int nx = static_cast<int>((lat.x1 - lat.x0) / pitch) + 1, ny = static_cast<int>((lat.y1 - lat.y0) / pitch) + 1;
    const auto grid = route::rasterise(region, lat, pitch, nx, ny);
    std::size_t inside = 0, checked = 0, skipped = 0;
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) {
        const analysis::detail::Pt p{static_cast<double>(lat.x0 + x * pitch), static_cast<double>(lat.y0 + y * pitch)};
        analysis::detail::Pt q;
        // On the boundary (within 5 nm) the two rules for the edge case may differ: the cost map does not care which side of an edge a centre falls on.
        if (reference.nearest_boundary_linear(p, 5.0, q)) {
          ++skipped;
          continue;
        }
        ++checked;
        const bool want = reference.island_of_linear(p) >= 0;
        REQUIRE((grid[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x)] != 0) == want);
        inside += want ? 1 : 0;
      }
    CHECK(checked > 100'000);
    CHECK(skipped < checked / 100);
    CHECK(inside > checked / 4);  // the plane and the outside are both well represented
    CHECK(inside < checked - checked / 20);
  }
}

TEST_CASE("critical: the reference map finds the plane a layer has next to it and the cells over its voids", "[critical]") {
  const auto l = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), plane(false));
  REQUIRE(l.board.zones.size() == 1);
  REQUIRE_FALSE(l.board.zones[0].fills.empty());
  const geom::Box lat{-20 * MM, -20 * MM, 20 * MM, 20 * MM};
  const Coord pitch = 100'000;
  const int n = 401;
  const auto m = route::build_reference_map(l.board, lat, pitch, n, n);
  REQUIRE(m.any());
  CHECK(m.reference_layer[0] == 1);  // the top layer's reference is the bottom one
  CHECK(m.reference_layer[1] == -1);  // the bottom layer is the plane itself: its other neighbour is not a plane
  CHECK(m.coverage[1] > 0.4);
  auto over = [&](double x, double y) { return m.over_void[static_cast<std::size_t>((y + 20.0) * 10 + 0.5) * static_cast<std::size_t>(n) + static_cast<std::size_t>((x + 20.0) * 10 + 0.5)] != 0; };
  CHECK_FALSE(over(-10, 0));  // over the left half
  CHECK_FALSE(over(10, 0));
  CHECK(over(0, 0));          // over the split
  CHECK(over(0, 10));
  CHECK(over(18, 0));         // beyond the plane's edge: no copper under it
  // The bottom layer is the plane and has none next to it: a track in the plane's own slot has no copper beside it, so its gaps are voids too.
  REQUIRE(m.over_void.size() == 2 * static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
  auto bottom_over = [&](double x, double y) { return m.over_void[static_cast<std::size_t>(n) * n + static_cast<std::size_t>((y + 20.0) * 10 + 0.5) * static_cast<std::size_t>(n) + static_cast<std::size_t>((x + 20.0) * 10 + 0.5)] != 0; };
  CHECK_FALSE(bottom_over(-10, 0));  // on its own copper
  CHECK(bottom_over(0, 0));          // in its own slot
  // No zone, no map.
  const auto bare = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), {});
  CHECK_FALSE(route::build_reference_map(bare.board, lat, pitch, n, n).any());
}

TEST_CASE("critical: a critical net goes round a split when there is a way, and only a critical one", "[critical][route]") {
  const auto l = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), plane(true));
  // Nothing asked: the shortest way, straight across the slot.
  auto plain = options();
  const auto r0 = run(l, plain);
  REQUIRE(r0.routed == r0.connections);
  const double straight = length_of(r0, l.board, "SIG");
  CHECK(audit(l, r0, "SIG").void_count >= 1);
  // Critical with a cost: round the slot's closed end, along the plane's solid strip. Longer, and over solid copper all the way.
  auto o = options();
  o.critical = {{"SIG", 1}};
  o.split_cost = 20;
  const auto r1 = run(l, o);
  REQUIRE(r1.routed == r1.connections);
  CHECK(length_of(r1, l.board, "SIG") > straight + 5.0);
  const auto a1 = audit(l, r1, "SIG");
  CHECK(a1.void_count == 0);
  CHECK(a1.split_count == 0);
  // A small cost is not worth the detour: the route stays straight and the audit reports the crossing.
  o.split_cost = 0.5;
  const auto r2 = run(l, o);
  REQUIRE(r2.routed == r2.connections);
  CHECK(length_of(r2, l.board, "SIG") < straight + 1.0);
  CHECK(audit(l, r2, "SIG").void_count >= 1);
  // The cost is for critical nets: the same board and cost, the net not named, is the plain route.
  auto other = options();
  other.critical = {{"NOT_THIS_NET", 1}};
  other.split_cost = 20;
  const auto r3 = run(l, other);
  CHECK(length_of(r3, l.board, "SIG") == length_of(r0, l.board, "SIG"));
  // Without the option the route is the one that was always made, byte for byte.
  const auto again = run(l, plain);
  REQUIRE(again.tracks.size() == r0.tracks.size());
  for (std::size_t i = 0; i < again.tracks.size(); ++i) {
    CHECK(again.tracks[i].a == r0.tracks[i].a);
    CHECK(again.tracks[i].b == r0.tracks[i].b);
  }
}

TEST_CASE("critical: with no way round a split the net crosses it and the audit says so, or stays unrouted when that is the rule", "[critical][route]") {
  const auto l = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), plane(false));  // two halves: no strip to go along
  auto o = options();
  o.critical = {{"SIG", 1}};
  o.split_cost = 20;
  const auto r = run(l, o);
  REQUIRE(r.routed == r.connections);  // the cost is finite: the connection is made
  const auto a = audit(l, r, "SIG");
  CHECK(a.void_count >= 1);
  CHECK(a.split_count >= 1);  // and the audit knows the return has no way across
  auto hard = o;
  hard.split_hard = true;
  const auto rh = run(l, hard);
  CHECK(length_of(rh, l.board, "SIG") == 0.0);  // a rule, not a cost: nothing crosses, the net stays unrouted
  CHECK(rh.routed == rh.connections - 1);        // the ground net, which is not critical, is routed
  // The same with a plane that has a way round: the hard rule finds it.
  const auto bridged = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), plane(true));
  const auto rb = run(bridged, hard);
  CHECK(rb.routed == rb.connections);
  CHECK(audit(bridged, rb, "SIG").void_count == 0);
}

TEST_CASE("critical: a critical net can leave a pad that sits in an antipad, even under the hard rule", "[critical][route]") {
  // The plane has a 2 x 2 mm antipad round each end of the net, as it does round a through-hole pad: the cells next to the pad are voids by construction.
  auto rings = plane(true);
  rings.push_back({{-11, -1}, {-11, 1}, {-9, 1}, {-9, -1}});  // holes are clockwise
  rings.push_back({{9, -1}, {9, 1}, {11, 1}, {11, -1}});
  const auto l = make_board(with_ground({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}), rings);
  auto o = options();
  o.critical = {{"SIG", 1}};
  o.split_cost = 20;
  o.split_hard = true;
  const auto r = run(l, o);
  CHECK(r.routed == r.connections);  // both nets: SIG leaves its pad and goes round the slot over copper
  CHECK(length_of(r, l.board, "SIG") > 25.0);
}

TEST_CASE("critical: a board without a reference plane routes as it always did", "[critical][route]") {
  const auto l = make_board({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}}, {});
  const auto r0 = run(l, options());
  auto o = options();
  o.critical = {{"SIG", 1}};
  o.split_cost = 20;
  o.critical_first = true;
  o.critical_via_mult = 3;
  const auto r1 = run(l, o);
  REQUIRE(r1.routed == r0.routed);
  REQUIRE(r1.tracks.size() == r0.tracks.size());
  for (std::size_t i = 0; i < r1.tracks.size(); ++i) {
    CHECK(r1.tracks[i].a == r0.tracks[i].a);
    CHECK(r1.tracks[i].b == r0.tracks[i].b);
  }
}

TEST_CASE("critical: critical nets are routed before the others, tier by tier", "[critical][route]") {
  // One layer's worth of conflict: A runs left to right (19.4 mm), B bottom to top (15.4 mm), and they cross. Shortest first routes B; whoever is
  // routed first keeps the crossing, the other has no way (the pads end 0.3 mm short of the board edge).
  json p;
  p["layerCount"] = 1;
  p["minTraceWidth"] = 0.1;
  p["nominalTraceWidth"] = 0.25;
  p["defaultObstacleMargin"] = 0.1;
  p["bounds"] = {{"minX", -10.0}, {"maxX", 10.0}, {"minY", -8.0}, {"maxY", 8.0}};
  json ob = json::array(), cn = json::array();
  auto pad = [&](const std::string& id, const std::string& part, double x, double y) {
    ob.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", 0.6}, {"height", 0.5}, {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", part}});
  };
  auto net = [&](const std::string& n, const std::string& a, double ax, double ay, const std::string& b, double bx, double by) {
    cn.push_back({{"name", n}, {"pointsToConnect", {{{"x", ax}, {"y", ay}, {"layer", "top"}, {"pointId", a}}, {{"x", bx}, {"y", by}, {"layer", "top"}, {"pointId", b}}}}, {"rootConnectionName", n}});
  };
  pad("a1", "U1", -9.7, 0); pad("a2", "U2", 9.7, 0); pad("b1", "U3", 0, -7.7); pad("b2", "U4", 0, 7.7);
  net("NET_A", "a1", -9.7, 0, "a2", 9.7, 0);
  net("NET_B", "b1", 0, -7.7, "b2", 0, 7.7);
  p["obstacles"] = ob;
  p["connections"] = cn;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_critical_order_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "b.kicad_pcb") << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  Loaded l{io::read_board_file((dir / "b.kicad_pcb").string()).board, io::read_design_rules((dir / "b.kicad_pcb").string())};
  fs::remove_all(dir);
  auto routed_nets = [&](const route::RouterOptions& o) {
    std::set<std::string> s;
    for (const auto& t : run(l, o).tracks) s.insert(l.board.nets[static_cast<std::size_t>(t.net)].name);
    return s;
  };
  auto o = options();
  CHECK(routed_nets(o) == std::set<std::string>{"NET_B"});  // shortest first
  o.critical = {{"NET_A", 1}};
  CHECK(routed_nets(o) == std::set<std::string>{"NET_B"});  // named critical but not told to go first: the order is as before
  o.critical_first = true;
  CHECK(routed_nets(o) == std::set<std::string>{"NET_A"});  // critical first: A gets the crossing
  o.critical = {{"NET_A", 2}, {"NET_B", 1}};
  CHECK(routed_nets(o) == std::set<std::string>{"NET_B"});  // tier 1 before tier 2, whatever the length
  o.critical = {{"NET_A", 1}, {"NET_B", 2}};
  CHECK(routed_nets(o) == std::set<std::string>{"NET_A"});
  auto f = options();
  f.first_nets = {"NET_B"};
  f.critical = {{"NET_A", 1}};
  f.critical_first = true;
  CHECK(routed_nets(f) == std::set<std::string>{"NET_B"});  // --first-nets stay in front of the critical tiers
}

TEST_CASE("critical: a critical net pays more for a via and takes the longer way on one layer", "[critical][route]") {
  // A fixed bar of copper on the top layer stands across the net's way. The net can hop under it (two vias) or go round its end.
  const auto l = make_board({{"SIG", -10, 0, "top"}, {"SIG", 10, 0, "top"}, {"OTHER", -10, -3, "top"}, {"OTHER", 10, -3, "top"}}, {}, {{0.0, -1.0, 0.6, 20.0, 0}});
  auto o = options();
  const auto base = run(l, o);
  REQUIRE(base.routed == base.connections);
  const int base_vias = vias_of(base, l.board, "SIG");
  const double base_len = length_of(base, l.board, "SIG");
  auto c = options();
  c.critical = {{"SIG", 1}};
  c.critical_via_mult = 8;
  const auto crit = run(l, c);
  REQUIRE(crit.routed == crit.connections);
  INFO("vias " << base_vias << " -> " << vias_of(crit, l.board, "SIG") << ", length " << base_len << " -> " << length_of(crit, l.board, "SIG"));
  CHECK(base_vias == 2);                        // plain: it hops under the bar, two vias, the shortest way
  CHECK(vias_of(crit, l.board, "SIG") == 0);   // dear vias: it goes round the end of the bar on one layer, longer
  CHECK(length_of(crit, l.board, "SIG") > base_len + 4.0);
  CHECK(vias_of(crit, l.board, "OTHER") == vias_of(base, l.board, "OTHER"));  // the other net pays what it always paid
  // A multiplier of 1 is no change at all.
  auto one = options();
  one.critical = {{"SIG", 1}};
  one.critical_via_mult = 1.0;
  const auto same = run(l, one);
  CHECK(vias_of(same, l.board, "SIG") == base_vias);
  CHECK(length_of(same, l.board, "SIG") == base_len);
}

TEST_CASE("critical: roles make nets critical, in tiers, and a differential pair is tier 2", "[critical]") {
  const fs::path demo = fs::path(TM_SOURCE_DIR) / "bench/data/kicad/demos/royalblue54L_feather/RoyalBlue54L-Feather.kicad_pcb";
  if (!fs::exists(demo)) SKIP("the demo board is not here");
  const auto lb = io::read_board_file(demo.string());
  const auto& cat = crules::builtin_catalogue();
  const auto det = crules::detect(lb.board, cat, nullptr);
  const auto found = crules::critical_nets(lb.board, cat, det, {});
  REQUIRE_FALSE(found.empty());
  // The crystals' pins are clocks (tier 1); they come first.
  CHECK(found.front().tier == 1);
  CHECK(found.front().why.find("crystal") != std::string::npos);
  for (std::size_t i = 1; i < found.size(); ++i) CHECK(found[i - 1].tier <= found[i].tier);
  // Deterministic.
  const auto again = crules::critical_nets(lb.board, cat, det, {});
  REQUIRE(again.size() == found.size());
  for (std::size_t i = 0; i < found.size(); ++i) CHECK(again[i].net == found[i].net);
  // A named pair adds its halves at tier 2 unless a role claimed them at a lower tier.
  model::NetId a = 0, b = 0;
  for (const auto& n : lb.board.nets) {
    if (n.name == "VDD") a = n.id;
    if (n.name == "GND") b = n.id;
  }
  if (a > 0 && b > 0) {
    const auto with = crules::critical_nets(lb.board, cat, det, {{a, b}});
    CHECK(std::any_of(with.begin(), with.end(), [&](const crules::CriticalNet& c) { return c.net == a && c.tier == 2 && c.why == "differential pair"; }));
  }
  // The table names only categories and roles the catalogue has.
  for (const auto& cr : crules::critical_roles()) {
    const auto it = std::find_if(cat.categories.begin(), cat.categories.end(), [&](const crules::Category& c) { return c.id == cr.category; });
    REQUIRE(it != cat.categories.end());
    CHECK(std::any_of(it->roles.begin(), it->roles.end(), [&](const auto& r) { return r.first == cr.role; }));
    CHECK(cr.tier >= 1);
    CHECK(cr.tier <= 3);
  }
}
