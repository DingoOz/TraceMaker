// SPDX-License-Identifier: GPL-3.0-or-later
// Finishing the last connections (D155, doc 05 §63): the end-game retry, the pin rescue inside it and the plane fallback are
// deterministic, never lose a routed connection, and change nothing when off.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"

using namespace tmk;

namespace {

const std::string kBoard = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";

bool same_copper(const route::RouteResult& a, const route::RouteResult& b) {
  if (a.routed != b.routed || a.tracks.size() != b.tracks.size() || a.vias.size() != b.vias.size()) return false;
  for (std::size_t i = 0; i < a.tracks.size(); ++i)
    if (a.tracks[i].a != b.tracks[i].a || a.tracks[i].b != b.tracks[i].b || a.tracks[i].layer != b.tracks[i].layer ||
        a.tracks[i].net != b.tracks[i].net)
      return false;
  for (std::size_t i = 0; i < a.vias.size(); ++i)
    if (a.vias[i].pos != b.vias[i].pos || a.vias[i].net != b.vias[i].net) return false;
  return true;
}

}  // namespace

TEST_CASE("last mile: end-game retry and rescue are deterministic and only add connections", "[route][last-mile]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions base;
  base.work_budget = 3'000'000;
  base.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  base.gpu_device = -1;
  base.soft_zones = base.via_in_pad = base.vias_off_pads = true;
  const auto plain = route::Router(b, rules, base).run();

  // Off: the option values that mean "off" give the default run.
  auto off = base;
  off.endgame_frac = 0;
  off.rescue_radii_mm.clear();
  off.plane_informed = false;
  CHECK(same_copper(route::Router(b, rules, off).run(), plain));

  auto on = base;
  on.endgame_frac = 0.2;
  on.rescue_radii_mm = {2, 4, 8};
  on.plane_informed = true;
  const auto r1 = route::Router(b, rules, on).run();
  const auto r2 = route::Router(b, rules, on).run();
  CHECK(same_copper(r1, r2));
  CHECK(r1.endgame_tried == r2.endgame_tried);
  CHECK(r1.rescue_tried == r2.rescue_tried);

  // The retry starts from the best state and keeps a rescue only when it routes more, so it adds without taking away.
  auto eg = base;
  eg.endgame_frac = 0.2;
  const auto e = route::Router(b, rules, eg).run();
  CHECK(e.routed >= plain.routed);
  if (plain.routed < plain.connections) CHECK(e.endgame_tried > 0);
}
