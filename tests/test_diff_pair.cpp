// Differential pairs (M12, doc 05 §14): the pair rule (gap and width from net class, custom rules and the clearance
// KiCad requires between the halves), offset/miter geometry, and the coupled-share / gap / skew measurement.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "route/diff_pair.hpp"

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
  const auto cm = drc::build_copper(b);
  const drc::RuleEngine re(b, r, cm);
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
    const auto cm = drc::build_copper(b);
    const drc::RuleEngine re(b, r, cm);
    const auto pr = route::pair_rule(b, r, re, 1, 2);
    CHECK(pr.width == 180'000);
    CHECK(pr.gap == 150'000);  // KiCad relaxes the clearance between the halves to the diff-pair gap
    CHECK(pr.source == "net class");
  }
  {
    const auto b = two_nets("DP", "DM");
    const auto cm = drc::build_copper(b);
    const drc::RuleEngine re(b, r, cm);
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
  const auto cm = drc::build_copper(b);
  const drc::RuleEngine re(b, r, cm);
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
