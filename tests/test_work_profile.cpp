// SPDX-License-Identifier: GPL-3.0-or-later
// Work profile (doc 10 §2, D88): the counters agree with the charged budget and do not depend on the thread count.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"

using namespace tmk;

namespace {

const std::string kBoard = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";

bool same(const route::WorkCounts& a, const route::WorkCounts& b) {
  return a.expansions == b.expansions && a.flood_cells == b.flood_cells && a.searches == b.searches && a.cell_checks == b.cell_checks &&
         a.fixed_checks == b.fixed_checks && a.field_cells == b.field_cells && a.fields == b.fields && a.commits == b.commits && a.rips == b.rips;
}

}  // namespace

TEST_CASE("work profile: charged work equals counted expansions and flood cells, at any thread count", "[route][work]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions opt;
  opt.work_budget = 1'000'000;
  opt.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  opt.gpu_device = -1;
  const auto one = route::route_portfolio(b, rules, opt, 3, {}, 1);
  const auto three = route::route_portfolio(b, rules, opt, 3, {}, 3);
  REQUIRE(one.work.size() == 3);
  REQUIRE(three.work.size() == 3);
  for (std::size_t v = 0; v < 3; ++v)
    for (int p = 0; p < route::kWorkPhases; ++p) CHECK(same(one.work[v].phase[static_cast<std::size_t>(p)], three.work[v].phase[static_cast<std::size_t>(p)]));
  const auto t = one.best.work.total();
  CHECK(t.expansions + t.flood_cells == one.best.expansions);
  CHECK(one.best.work.phase[static_cast<std::size_t>(route::WorkPhase::FirstPass)].searches > 0);
}
