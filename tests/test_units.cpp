// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/parse_double.hpp"
#include "core/rng.hpp"
#include "core/units.hpp"

TEST_CASE("millimetre <-> nanometre conversion is exact for KiCad values", "[core][units]") {
  CHECK(tmk::mm_to_nm(1.0) == 1'000'000);
  CHECK(tmk::mm_to_nm(0.2) == 200'000);
  CHECK(tmk::mm_to_nm(-0.127) == -127'000);
  CHECK(tmk::mm_to_nm(0.0000005) == 1);  // half rounds away from zero
  CHECK(tmk::nm_to_mm(254'000) == 0.254);
}

// parse_double (core/parse_double.hpp, D135): the strtod_l path used on Apple platforms takes exactly what
// std::from_chars takes and gives the same double.
TEST_CASE("parse_double accepts from_chars' grammar only", "[core][units]") {
  using tmk::detail::parse_double_strtod;
  for (const char* s : {"1", "-0.127", "0.2", ".5", "5.", "1e3", "1E-3", "-2.5e+2", "0.0000005", "254000", "inf", "nan", "4.9e-324", "-2.2e-308"}) {
    INFO(s);
    REQUIRE(tmk::parse_double(s).has_value());
    CHECK(parse_double_strtod(s).has_value());
  }
  CHECK(*parse_double_strtod("-0.127") == -0.127);
  CHECK(*parse_double_strtod("1E-3") == 0.001);
  for (const char* s : {"", "-", "+1", " 1", "1 ", "0x10", "-0X1p3", "1,5", "1.5mm", "e3", "--1", "1e", "1e400", "-1e400", "1e-400"}) {
    INFO('"' << s << '"');
    CHECK_FALSE(tmk::parse_double(s).has_value());
    CHECK_FALSE(parse_double_strtod(s).has_value());
  }
}

#if !defined(__APPLE__)
TEST_CASE("parse_double: the strtod_l path equals from_chars bit for bit", "[core][units]") {
  // Random strings over the characters numbers are made of, plus decimal renderings of random doubles.
  const tmk::RngStream rng(0x9A45E, 0xD0B1u, 0);
  const std::string alphabet = "0123456789.-+eE xn";
  for (std::uint64_t n = 0; n < 20000; ++n) {
    const tmk::U32x4 b = rng.block(n);
    std::string s;
    if (n % 2 == 0) {
      for (std::uint32_t k = 0; k < 1 + b.v[0] % 12; ++k) s += alphabet[(b.v[1 + k % 3] >> (k % 4 * 8)) % alphabet.size()];
    } else {
      const std::uint64_t bits = (std::uint64_t{b.v[0]} << 32 | b.v[1]) & 0x7FEF'FFFF'FFFF'FFFFull;  // finite
      double d = 0;
      std::memcpy(&d, &bits, sizeof d);
      char buf[512];  // %f of the largest double is 309 digits before the point
      if (b.v[2] % 2) std::snprintf(buf, sizeof buf, "%.17g", d);
      else std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(b.v[3] % 9), d);
      s = (b.v[3] & 0x100u ? "-" : "") + std::string(buf);
    }
    INFO('"' << s << '"');
    const auto a = tmk::detail::parse_double_strtod(s);
    const auto f = tmk::detail::parse_double_from_chars(s);
    REQUIRE(a.has_value() == f.has_value());
    if (a && !std::isnan(*a)) CHECK(std::memcmp(&*a, &*f, sizeof(double)) == 0);
  }
}
#endif
