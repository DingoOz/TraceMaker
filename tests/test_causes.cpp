// SPDX-License-Identifier: GPL-3.0-or-later
// Why connections stay open (doc 05 §55, M20 C5): `tracemaker causes` names the copper that has to move.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/causes.hpp"
#include "io/kicad/board_reader.hpp"
#include "route/router.hpp"
#include "sexpr/sexpr.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, two layers. S joins U1.1 and R2.1; a square of net T on both layers encloses U1.1, so S cannot leave it. T has its one pad
// on U1 (`own`: the package's own fan-out) or on a part far away (through traffic).
std::string board(bool own) {
  std::string ring;
  for (const char* layer : {"F.Cu", "B.Cu"})
    for (const char* seg : {"(start 4 9) (end 6 9)", "(start 6 9) (end 6 11)", "(start 6 11) (end 4 11)", "(start 4 11) (end 4 9)"})
      ring += std::string("  (segment ") + seg + " (width 0.25) (layer " + layer + ") (net 2))\n";
  const std::string t_pad = "(pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 2 T))";
  std::string s = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (pad_to_mask_clearance 0))
  (net 0 "")
  (net 1 S)
  (net 2 T)
  (module T:U (layer F.Cu) (at 5 10)
    (fp_text reference U1 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))";
  if (own) s += "\n    (pad 2 smd rect (at 0 3) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 2 T))";
  s += R"()
  (module T:R (layer F.Cu) (at 25 10)
    (fp_text reference R2 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))
)";
  if (!own) s += "  (module T:X (layer F.Cu) (at 15 17)\n    (fp_text reference X9 (at 0 0) (layer F.SilkS))\n    " + t_pad + ")\n";
  s += ring + R"(  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";
  return s;
}

struct TempDir {
  fs::path dir;
  TempDir() : dir(fs::temp_directory_path() / ("tm_causes_" + std::to_string(::getpid()))) { fs::create_directories(dir); }
  ~TempDir() { fs::remove_all(dir); }
};

}  // namespace

TEST_CASE("causes: copper of a net that passes through, or of the package's own pins, is named", "[causes]") {
  TempDir d;
  app::CausesOptions o;
  o.baseline_work = 300'000;
  o.work = 300'000;
  o.threads = 2;
  for (const bool own : {false, true}) {
    const fs::path p = d.dir / (own ? "own.kicad_pcb" : "through.kicad_pcb");
    std::ofstream(p) << board(own);
    const auto j = app::classify_open_connections(p.string(), o, false);
    INFO(j.dump(1));
    REQUIRE(j["baseline"]["open"] == 1);
    REQUIRE(j["connections"].size() == 1);
    const auto& c = j["connections"][0];
    CHECK(c["net"] == "S");
    CHECK(c["cause"] == (own ? "package_fanout" : "through_traffic"));
    // The copies tried, in order: the cause is the first on which S routes.
    CHECK(c["copies"] == (own ? "through_traffic,package_fanout" : "through_traffic"));
  }
  // The same board and options give the same report, whatever the threads.
  const fs::path p = d.dir / "through.kicad_pcb";
  o.threads = 1;
  const auto a = app::classify_open_connections(p.string(), o, false);
  o.threads = 4;
  CHECK(app::classify_open_connections(p.string(), o, false) == a);
}

// --crowded-source: U1.1 sits in a 3 x 3 grid of its part's pads that have no net, too close together for a full-width track between
// them. A search from R2.1 (the connection's first end: R2 comes first in the file) floods the board before it finds the one way in; a
// search from U1.1 finds the way out at once. Both route S; the work differs by two orders of magnitude.
TEST_CASE("route --crowded-source starts a strict search from the crowded end", "[causes][route]") {
  std::string grid;
  for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
      if (x || y)
        grid += "\n    (pad " + std::to_string(10 + 3 * (x + 1) + y + 1) + " smd rect (at " + std::to_string(x) + " " + std::to_string(y) +
                ") (size 0.5 0.5) (layers F.Cu F.Paste F.Mask))";
  const std::string text = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (pad_to_mask_clearance 0))
  (net 0 "")
  (net 1 S)
  (module T:R (layer F.Cu) (at 25 10)
    (fp_text reference R2 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 0.5 0.5) (layers F.Cu F.Paste F.Mask) (net 1 S)))
  (module T:U (layer F.Cu) (at 5 10)
    (fp_text reference U1 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 0.5 0.5) (layers F.Cu F.Paste F.Mask) (net 1 S)))" + grid + R"()
  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";
  const auto brd = io::read_board(sexpr::Document::parse(text));
  model::DesignRules r;
  model::NetClass d;
  d.name = "Default";
  d.clearance = 200'000;
  d.track_width = 250'000;
  d.via_diameter = 800'000;
  d.via_drill = 400'000;
  r.classes = {d};
  auto run = [&](bool crowded) {
    route::RouterOptions o;
    o.work_budget = 2'000'000;
    o.gpu_device = -1;
    o.crowded_source = crowded;
    return route::Router(brd, r, o).run();
  };
  const auto plain = run(false), crowded = run(true);
  CHECK(plain.connections == 1);
  CHECK(plain.routed == 1);
  CHECK(crowded.routed == 1);
  CHECK(crowded.expansions * 10 < plain.expansions);
}
