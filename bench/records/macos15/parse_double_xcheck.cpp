// SPDX-License-Identifier: GPL-3.0-or-later
// The macOS path of tmk::parse_double (strtod_l, D135) against libstdc++'s std::from_chars, which Apple's libc++ lacks
// before macOS 26. Built on a Mac with Homebrew GCC, so __APPLE__ selects the strtod_l path:
//   g++-16 -std=c++20 -O2 -I src bench/records/macos15/parse_double_xcheck.cpp -o xcheck && ./xcheck
// Exit status 1 on any mismatch (accepted by one and not the other, or a different double).
#include <charconv>
#include <cstdio>
#include <cstring>
#include <random>
#include "core/parse_double.hpp"
int main() {
  std::mt19937_64 g(1);
  const std::string alphabet = "0123456789.-+eE xnaif";
  long n_ok = 0, n_val = 0, bad = 0;
  for (long n = 0; n < 2'000'000; ++n) {
    std::string s;
    if (n % 2 == 0) {
      for (unsigned k = 0, len = 1 + g() % 14; k < len; ++k) s += alphabet[g() % alphabet.size()];
    } else {
      std::uint64_t bits = g() & 0x7FEF'FFFF'FFFF'FFFFull; double d; std::memcpy(&d, &bits, 8);
      char buf[512]; int mode = g() % 3;
      if (n % 1000 == 1) std::snprintf(buf, sizeof buf, "%de-%d", int(g() % 10), int(300 + g() % 40));
      else if (mode == 0) std::snprintf(buf, sizeof buf, "%.17g", d);
      else if (mode == 1) std::snprintf(buf, sizeof buf, "%.*f", int(g() % 9), double(int64_t(g() % 2000000000) - 1000000000) / 1e6);
      else std::snprintf(buf, sizeof buf, "%.*g", int(1 + g() % 25), d);
      s = (g() & 1 ? "-" : "") + std::string(buf);
    }
    auto a = tmk::detail::parse_double_strtod(s);
    double v = 0; auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    bool f = r.ec == std::errc{} && r.ptr == s.data() + s.size();
    if (a.has_value() != f || (f && v == v && std::memcmp(&*a, &v, 8) != 0)) { if (++bad < 10) std::printf("MISMATCH \"%s\" strtod=%d from_chars=%d\n", s.c_str(), a.has_value(), f); }
    n_val += f; ++n_ok;
  }
  std::printf("%ld strings, %ld valid numbers, %ld mismatches\n", n_ok, n_val, bad);
  return bad != 0;
}
