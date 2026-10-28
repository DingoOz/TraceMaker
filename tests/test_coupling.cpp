// SPDX-License-Identifier: GPL-3.0-or-later
// Coupled segments of a differential pair as KiCad 10.0.6 finds them (doc 05 §59). The DRC's verdicts on whole boards are in the rule
// parity corpus (tests/integration/rule_parity, cases diff_pair_*).
#include <catch2/catch_test_macros.hpp>

#include "drc/coupling.hpp"

using namespace tmk;
using model::Point;

TEST_CASE("coupling: parallel tracks couple over their common stretch, edge to edge", "[drc][coupling]") {
  // P from x 0 to 10 mm, N from 4 to 14 mm, 0.5 mm apart, 0.2 mm wide: coupled over x 4..10, gap 0.3 mm.
  const auto c = drc::couple({0, 0}, {10'000'000, 0}, 200'000, {4'000'000, 500'000}, {14'000'000, 500'000}, 200'000);
  REQUIRE(c);
  CHECK(c->gap == 300'000);
  CHECK(c->length == 6'000'000);
  CHECK(c->pa == Point{4'000'000, 0});
  CHECK(c->pb == Point{10'000'000, 0});
  // The direction of either track does not matter.
  const auto r = drc::couple({10'000'000, 0}, {0, 0}, 200'000, {14'000'000, 500'000}, {4'000'000, 500'000}, 200'000);
  REQUIRE(r);
  CHECK(r->gap == 300'000);
  CHECK(r->length == 6'000'000);
  // Diagonals: 45 degree tracks 0.5 mm apart measured square to them.
  const auto d = drc::couple({0, 0}, {5'000'000, 5'000'000}, 200'000, {500'000, 0}, {5'500'000, 5'000'000}, 200'000);
  REQUIRE(d);
  CHECK(d->gap == 353'553 - 200'000);
}

TEST_CASE("coupling: tracks that meet as lines, lie on one line or do not overlap are not coupled", "[drc][coupling]") {
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {0, 500'000}, {10'000'000, 600'000}, 200'000));  // not quite parallel
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {12'000'000, 0}, {20'000'000, 0}, 200'000));     // collinear
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {10'000'000, 500'000}, {20'000'000, 500'000}, 200'000));  // end to end
  CHECK_FALSE(drc::couple({0, 0}, {1, 1}, 200'000, {0, 500'000}, {10'000'000, 500'000}, 200'000));          // about 1 nm long
}
