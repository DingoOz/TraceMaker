// Escape planning (M9): corridor geometry on a synthetic BGA and the feasibility analysis on a fixture board.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <set>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/escape.hpp"
#include "route/obstacles.hpp"

using namespace tmk;
using geom::Point;

namespace {

// n x n ball grid at `pitch`, every ball on its own net, on F.Cu of a 2-layer board.
model::Board bga(int n, Coord pitch, Coord ball) {
  model::Board b;
  b.nets.push_back({});
  b.footprints.push_back({});
  b.footprints[0].reference = "U1";
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      model::Pad p;
      p.footprint = 0;
      p.number = std::to_string(y * n + x);
      p.type = model::PadType::Smd;
      p.shape = model::PadShape::Circle;
      p.pos = {x * pitch, y * pitch};
      p.size_x = p.size_y = ball;
      p.copper = model::layer_bit(0);
      model::Net net;
      net.name = "N" + p.number;
      p.net = static_cast<model::NetId>(b.nets.size());
      b.nets.push_back(net);
      b.footprints[0].pads.push_back(static_cast<int>(b.pads.size()));
      b.pads.push_back(p);
    }
  return b;
}

double dist_point_segment(Point p, Point a, Point b) {
  const double ux = static_cast<double>(b.x - a.x), uy = static_cast<double>(b.y - a.y);
  const double len2 = ux * ux + uy * uy;
  double t = len2 > 0 ? ((static_cast<double>(p.x - a.x)) * ux + (static_cast<double>(p.y - a.y)) * uy) / len2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(static_cast<double>(p.x - a.x) - t * ux, static_cast<double>(p.y - a.y) - t * uy);
}

}  // namespace

TEST_CASE("escape plan: perimeter pins fan out, inner balls get one dog-bone site each", "[escape]") {
  const Coord pitch = 800'000;
  const auto b = bga(6, pitch, 400'000);
  std::vector<char> needs(b.pads.size(), 1);
  route::EscapeStats st;
  const auto plan = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, {}, &st);
  REQUIRE(plan.size() == b.pads.size());
  CHECK(st.parts == 1);
  CHECK(st.perimeter == 20);  // the outer ring of a 6 x 6 array
  CHECK(st.dogbones == 16);
  std::set<std::pair<Coord, Coord>> sites;
  const Point centre{5 * pitch / 2, 5 * pitch / 2};
  for (const auto& c : plan) {
    CHECK(c.band <= pitch / 2);
    if (c.via) {
      CHECK(sites.insert({c.b.x, c.b.y}).second);  // every interstitial site serves one ball
      // pointing away from the package centre
      CHECK(std::llabs(c.b.x - centre.x) >= std::llabs(c.a.x - centre.x));
      CHECK(std::llabs(c.b.y - centre.y) >= std::llabs(c.a.y - centre.y));
    } else {
      // leaves the package
      const bool out = c.b.x < 0 || c.b.y < 0 || c.b.x > 5 * pitch || c.b.y > 5 * pitch;
      CHECK(out);
    }
  }
  // No corridor runs over another ball's centre (corridors never reserve a neighbour's pad).
  for (const auto& c : plan)
    for (const auto& p : b.pads)
      if (b.pads[static_cast<std::size_t>(c.pad)].pos != p.pos) CHECK(dist_point_segment(p.pos, c.a, c.b) >= static_cast<double>(pitch) / 2 - 1);
  // Deterministic.
  const auto again = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; });
  REQUIRE(again.size() == plan.size());
  for (std::size_t i = 0; i < plan.size(); ++i) CHECK((again[i].b == plan[i].b && again[i].pad == plan[i].pad));
  // Pads that need no routing get no corridor; coarse parts are left alone.
  std::vector<char> none(b.pads.size(), 0);
  CHECK(route::plan_escapes(b, none, [](model::NetId) { return Coord{300'000}; }).empty());
  route::EscapeOptions coarse;
  coarse.max_pitch = 500'000;
  CHECK(route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, coarse).empty());
}

TEST_CASE("escape analysis: sbc's DRAM balls are blocked only by the solder-mask rule", "[escape][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/sbc_sbc/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  model::Board b = lb.board;
  route::Obstacles obs(b, rules);
  const auto parts = route::analyse_escapes(b, rules, obs);
  const route::PartEscape* dram = nullptr;
  for (const auto& pe : parts)
    if (pe.ref == "DRAM1") dram = &pe;
  REQUIRE(dram != nullptr);
  CHECK(dram->pins > 60);
  CHECK(!dram->dead.empty());
  int mask_only = 0;
  for (const auto& d : dram->dead) mask_only += d.reason.find("solder-mask") != std::string::npos;
  CHECK(mask_only > 0);
  CHECK(!dram->hint.empty());
  // With tented vias the same balls escape: the analysis restores the setting it probes with.
  CHECK(obs.via_mask() > 0);
  obs.set_via_mask(0);
  const auto tented = route::analyse_escapes(b, rules, obs);
  for (const auto& pe : tented)
    if (pe.ref == "DRAM1") CHECK(pe.dead.size() < dram->dead.size());
}
