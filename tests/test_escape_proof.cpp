// Escape proof of an open pin (analysis/escape_proof.hpp, doc 20 §2.4): a 0.36 mm pin in a ring of eight 0.7 mm pads of other
// nets, rules 0.12 mm tracks (board minimum 0.05 mm), 0.1 mm clearance. A track needs a gap of its width + 2 x 0.1 mm between two
// ring pads.
//  - pitch 0.8 mm, 0.4 mm vias: gaps of 0.1 mm, no track gets out, and a via keeps 0.3 mm from the ring only inside the pin's pad;
//  - pitch 1.0 mm, 0.8 mm vias: gaps of 0.3 mm, walled in for the class's 0.12 mm track, but a 0.1 mm track gets out (a neck-down),
//    so not enclosed; with the board minimum at 0.12 mm it is;
//  - pitch 1.1 mm: gaps of 0.4 mm, the pin is not walled in at all.
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "analysis/escape_proof.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"

using namespace tmk;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Ring {
  model::Board b;
  model::DesignRules rules;
  int pin = -1;
};

Ring make_ring(double pitch, double via = 0.4) {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -4.0}, {"maxX", 8.0}, {"minY", -4.0}, {"maxY", 4.0}};
  json obstacles = json::array(), connections = json::array();
  auto pad = [&](const std::string& id, double x, double y, double size) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", size}, {"height", size},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", "c"}});
  };
  auto conn = [&](const std::string& name, const std::string& a, double ax, double ay, const std::string& b, double bx, double by) {
    connections.push_back({{"name", name},
                           {"pointsToConnect",
                            {{{"x", ax}, {"y", ay}, {"layer", "top"}, {"pointId", a}}, {{"x", bx}, {"y", by}, {"layer", "top"}, {"pointId", b}}}},
                           {"rootConnectionName", name}});
  };
  pad("pin", 0, 0, 0.36);
  pad("far", 6, 0, 0.36);
  conn("c_pin", "pin", 0, 0, "far", 6, 0);
  int k = 0;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      const std::string id = "r" + std::to_string(k), fid = "f" + std::to_string(k);
      pad(id, dx * pitch, dy * pitch, 0.7);
      pad(fid, 6.5, -3 + 0.6 * k, 0.3);
      conn("c" + std::to_string(k), id, dx * pitch, dy * pitch, fid, 6.5, -3 + 0.6 * k);
      ++k;
    }
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_escape_proof_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "b.kicad_pcb") << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Ring r;
  r.b = io::read_board_file((dir / "b.kicad_pcb").string()).board;
  r.rules = io::read_design_rules((dir / "b.kicad_pcb").string());
  fs::remove_all(dir);
  for (auto& cl : r.rules.classes) cl.clearance = mm_to_nm(0.1), cl.track_width = mm_to_nm(0.12), cl.via_diameter = mm_to_nm(via), cl.via_drill = mm_to_nm(0.2);
  r.rules.minimums.clearance = mm_to_nm(0.1);
  r.rules.minimums.track_width = mm_to_nm(0.05);
  r.rules.minimums.via_diameter = mm_to_nm(via);
  for (std::size_t i = 0; i < r.b.pads.size(); ++i)
    if (r.b.pads[i].pos.x == 0 && r.b.pads[i].pos.y == 0) r.pin = static_cast<int>(i);
  REQUIRE(r.pin >= 0);
  return r;
}

}  // namespace

TEST_CASE("escape proof: a pin walled in by a tight ring, a via only in its pad", "[escape_proof]") {
  const Ring r = make_ring(0.8);
  const auto e = analysis::EscapeAnalyser(r.b, r.rules).run(r.pin);
  REQUIRE(e.analysed);
  CHECK(e.enclosed);
  CHECK(e.layers_enclosed == std::vector<int>{0});
  CHECK(e.neck_width == 0);
  CHECK(e.via_in_pad_fits);
  CHECK(e.walls.size() >= 4);  // the four nearest bound the space; the diagonal ones may lie behind their keep-outs
  CHECK(e.text.find("cannot be connected as placed") != std::string::npos);
  // With vias allowed in the pad, it is no longer enclosed.
  analysis::EscapeOptions vip;
  vip.via_in_pad = true;
  CHECK_FALSE(analysis::EscapeAnalyser(r.b, r.rules, vip).run(r.pin).enclosed);
}

TEST_CASE("escape proof: a neck-down gets out of a ring with 0.3 mm gaps", "[escape_proof]") {
  Ring r = make_ring(1.0, 0.8);
  const auto e = analysis::EscapeAnalyser(r.b, r.rules).run(r.pin);
  CHECK_FALSE(e.enclosed);
  CHECK(e.layers_enclosed == std::vector<int>{0});
  CHECK(e.neck_width >= mm_to_nm(0.095));
  CHECK(e.neck_width <= mm_to_nm(0.115));  // 0.1 mm exactly, plus the grid's slack
  CHECK(e.text.find("neck-down") != std::string::npos);
  r.rules.minimums.track_width = mm_to_nm(0.12);  // no narrower track allowed: now a proof
  const auto p = analysis::EscapeAnalyser(r.b, r.rules).run(r.pin);
  CHECK(p.enclosed);
  CHECK(p.via_in_pad_fits);  // a 0.8 mm via keeps 0.5 mm from the ring (edge at 0.65 mm) only within 0.15 mm of the centre: in the pad
}

TEST_CASE("escape proof: a ring with 0.4 mm gaps does not enclose the pin", "[escape_proof]") {
  const Ring r = make_ring(1.1);
  const auto e = analysis::EscapeAnalyser(r.b, r.rules).run(r.pin);
  CHECK(e.analysed);
  CHECK_FALSE(e.enclosed);
  CHECK_FALSE(e.soft_only);
}
