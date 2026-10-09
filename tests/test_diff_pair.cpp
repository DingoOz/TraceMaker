// SPDX-License-Identifier: GPL-3.0-or-later
// Differential pairs (M12, doc 05 §15): the pair rule (gap and width from net class, custom rules and the clearance
// KiCad requires between the halves), offset/miter geometry, and the coupled-share / gap / skew measurement.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "io/kicad/board_reader.hpp"
#include "route/diff_pair.hpp"
#include "route/router.hpp"

using namespace tmk;
using geom::Point;

namespace {

model::Board two_nets(const std::string& a, const std::string& b) {
  model::Board bd;
  bd.nets.push_back({});
  model::Net n;
  n.id = 1;
  n.name = a;
  bd.nets.push_back(n);
  n.id = 2;
  n.name = b;
  bd.nets.push_back(n);
  return bd;
}

model::DesignRules rules_with(Coord clearance, bool dp, Coord dp_width, Coord dp_gap) {
  model::DesignRules r;
  model::NetClass k;
  k.name = "Default";
  k.clearance = clearance;
  k.track_width = 250'000;
  k.has_diff_pair_gap = dp;
  k.diff_pair_width = dp_width;
  k.diff_pair_gap = dp_gap;
  r.classes.push_back(k);
  return r;
}

double dist(Point a, Point b) { return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y)); }

}  // namespace

TEST_CASE("pair rule: tightest legal coupling without diff-pair settings", "[diffpair]") {
  const auto b = two_nets("USB_DP", "USB_DM");  // not a KiCad pair by name: the full clearance applies
  const auto r = rules_with(200'000, false, 0, 0);
  const drc::RuleEngine re(b, r);
  const auto pr = route::pair_rule(b, r, re, 1, 2);
  CHECK(pr.width == 250'000);
  CHECK(pr.gap == 200'000);
  CHECK(pr.required_gap == 200'000);
  CHECK(pr.source == "clearance");
  CHECK(2 * pr.offset - pr.width >= pr.gap);           // offsets never undercut the gap
  CHECK(2 * pr.offset - pr.width <= pr.gap + 3'000);   // and add only the rounding margin
  CHECK(2 * pr.via_offset - pr.via_diameter >= pr.required_gap);
}

TEST_CASE("pair rule: net-class diff-pair width and gap, relaxed clearance only for KiCad pairs", "[diffpair]") {
  const auto r = rules_with(200'000, true, 180'000, 150'000);
  {
    const auto b = two_nets("/TMDS_0_P", "/TMDS_0_N");
    const drc::RuleEngine re(b, r);
    const auto pr = route::pair_rule(b, r, re, 1, 2);
    CHECK(pr.width == 180'000);
    CHECK(pr.gap == 150'000);  // KiCad relaxes the clearance between the halves to the diff-pair gap
    CHECK(pr.source == "net class");
  }
  {
    const auto b = two_nets("DP", "DM");
    const drc::RuleEngine re(b, r);
    const auto pr = route::pair_rule(b, r, re, 1, 2);
    CHECK(pr.gap == 200'000);  // not a pair to KiCad: the gap may not undercut the clearance
  }
}

TEST_CASE("pair rule: a custom diff_pair_gap rule wins, diff_pair_uncoupled limits the legs", "[diffpair]") {
  auto r = rules_with(150'000, false, 0, 0);
  model::CustomRule cr;
  cr.name = "pairs";
  model::Constraint g;
  g.type = "diff_pair_gap";
  g.opt = 300'000;
  model::Constraint u;
  u.type = "diff_pair_uncoupled";
  u.max = 4'000'000;
  cr.constraints = {g, u};
  r.custom.push_back(cr);
  const auto b = two_nets("D+", "D-");
  const drc::RuleEngine re(b, r);
  const auto pr = route::pair_rule(b, r, re, 1, 2);
  CHECK(pr.gap == 300'000);
  CHECK(pr.source == "rule");
  REQUIRE(pr.max_uncoupled);
  CHECK(*pr.max_uncoupled == 4'000'000);
}

TEST_CASE("pair geometry: offsets are symmetric and miters keep the offset distance", "[diffpair]") {
  const Point c{1'000'000, 2'000'000};
  for (int d = 0; d < 8; ++d) {
    const Point l = route::offset_point(c, route::left_normal(d), 300'000), r = route::offset_point(c, route::left_normal(d), -300'000);
    CHECK(std::fabs(dist(l, r) - 600'000) <= 2);
    CHECK(l.x - c.x == c.x - r.x);
    CHECK(l.y - c.y == c.y - r.y);
    for (int t : {1, 7}) {
      const int d2 = (d + t) & 7;
      const Point m = route::miter_point(c, d, d2, 300'000);
      // The corner lies on both offset lines: its distance to each centreline direction's line is the offset.
      for (int dd : {d, d2}) {
        const auto u = route::unit_dir(dd);
        const double cross = static_cast<double>(m.x - c.x) * u.y - static_cast<double>(m.y - c.y) * u.x;
        CHECK(std::fabs(std::fabs(cross) - 300'000) <= 2);
      }
    }
  }
}

TEST_CASE("pair measurement: coupled share, gap and skew", "[diffpair]") {
  std::vector<model::Track> t;
  // Two parallel 10 mm tracks, 0.25 mm wide, edges 0.2 mm apart, then net 1 runs on alone for 2 mm.
  t.push_back({{0, 0}, {10'000'000, 0}, 250'000, 0, 1, false, sexpr::kNoNode});
  t.push_back({{0, 450'000}, {10'000'000, 450'000}, 250'000, 0, 2, false, sexpr::kNoNode});
  t.push_back({{10'000'000, 0}, {12'000'000, 0}, 250'000, 0, 1, false, sexpr::kNoNode});
  // A crossing track of net 2 on the other layer does not count.
  t.push_back({{5'000'000, -1'000'000}, {5'000'000, 1'000'000}, 250'000, 1, 2, false, sexpr::kNoNode});
  const auto st = route::measure_pair(t, {}, 1, 2, 260'000);
  CHECK(std::fabs(st.length_a - 12e6) < 1);
  CHECK(std::fabs(st.length_b - 12e6) < 1);
  // Distance is to the partner segment, its ends included: a half running on past the partner's end still counts
  // while within the threshold of that end (here 0.24 mm), which keeps miter corners from counting as uncoupled.
  CHECK(st.coupled_a >= 10e6 - 1e4);
  CHECK(st.coupled_a <= 10.3e6);
  CHECK(std::fabs(st.coupled_b - 10e6) < 1e4);
  CHECK(std::fabs(st.gap_median - 200'000) < 1);
  CHECK(std::fabs(st.gap_min - 200'000) < 1);
  CHECK(st.coupled_share() >= 20.0 / 24.0 - 1e-3);
  CHECK(st.coupled_share() <= 20.3 / 24.0);
  CHECK(st.skew() < 1);
  // Farther apart than the threshold: nothing coupled.
  const auto far = route::measure_pair(t, {}, 1, 2, 150'000);
  CHECK(far.coupled_a == 0);
  CHECK(far.gap_median == 0);
}

namespace {

// 40 x 20 mm, one copper layer (no via can hop the fence). Pair D_P/D_N runs from U1 (x = 5) to U2 (x = 35); a locked
// ring of net W surrounds both pads of each end, so no pair or single route gets out.
model::Board fenced_pair() {
  std::string s = R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "D_P") (net 2 "D_N") (net 3 "W")
)";
  for (const int x : {5, 35}) {
    const std::string ref = x == 5 ? "U1" : "U2", at = std::to_string(x);
    s += "  (footprint \"T:P\" (layer \"F.Cu\") (at " + at + " 10) (property \"Reference\" \"" + ref + "\")\n"
         "    (pad \"1\" smd rect (at 0 -0.3) (size 0.3 0.3) (layers \"F.Cu\") (net 1 \"D_P\"))\n"
         "    (pad \"2\" smd rect (at 0 0.3) (size 0.3 0.3) (layers \"F.Cu\") (net 2 \"D_N\")))\n";
    const double x0 = x - 0.75, x1 = x + 0.75;
    for (const auto& [a, b] : {std::pair{std::pair{x0, 9.0}, std::pair{x1, 9.0}}, std::pair{std::pair{x1, 9.0}, std::pair{x1, 11.0}},
                               std::pair{std::pair{x1, 11.0}, std::pair{x0, 11.0}}, std::pair{std::pair{x0, 11.0}, std::pair{x0, 9.0}}})
      s += "  (segment (start " + std::to_string(a.first) + " " + std::to_string(a.second) + ") (end " + std::to_string(b.first) + " " +
           std::to_string(b.second) + ") (width 0.2) (locked yes) (layer \"F.Cu\") (net 3))\n";
  }
  s += "  (gr_rect (start 0 0) (end 40 20) (stroke (width 0.1) (type default)) (fill none) (layer \"Edge.Cuts\"))\n)\n";
  return io::read_board(sexpr::Document::parse(s));
}

route::RouteResult route_fenced(const model::Board& b, long work) {
  route::RouterOptions o;
  o.work_budget = work;
  o.time_limit_s = 600;
  o.gpu_device = -1;
  o.pair_nets = {{b.net_by_name("D_P"), b.net_by_name("D_N")}};
  return route::Router(b, rules_with(200'000, false, 0, 0), o).run();
}

}  // namespace

TEST_CASE("pair search: rejected candidates are work, and the work budget stops an attempt", "[diffpair][route]") {
  const auto b = fenced_pair();
  const auto full = route_fenced(b, 0);
  CHECK(full.routed == 0);
  // Closed states are few (no leg is legal, so no state is ever reached); the charge is the leg checks of the
  // thousands of rejected candidates. Charging only closed states left such attempts at about zero work.
  CHECK(full.pair_work > 5'000);
  CHECK(full.expansions >= full.pair_work);
  // A budget far below the attempt's work stops it inside the attempt, within one check interval (64 candidates of at
  // most 72 leg checks each), and the same budget gives the same run.
  const long budget = 2'000;
  const auto cut = route_fenced(b, budget);
  CHECK(cut.pair_work < full.pair_work);
  CHECK(cut.expansions <= budget + 64 * 72);
  const auto again = route_fenced(b, budget);
  CHECK(again.expansions == cut.expansions);
  CHECK(again.pair_work == cut.pair_work);
}
