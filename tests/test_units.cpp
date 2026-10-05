// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/units.hpp"

TEST_CASE("millimetre <-> nanometre conversion is exact for KiCad values", "[core][units]") {
  CHECK(tmk::mm_to_nm(1.0) == 1'000'000);
  CHECK(tmk::mm_to_nm(0.2) == 200'000);
  CHECK(tmk::mm_to_nm(-0.127) == -127'000);
  CHECK(tmk::mm_to_nm(0.0000005) == 1);  // half rounds away from zero
  CHECK(tmk::nm_to_mm(254'000) == 0.254);
}
