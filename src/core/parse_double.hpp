// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <xlocale.h>
#else
#include <charconv>
#include <locale.h>
#endif

namespace tmk {

namespace detail {

// strtod_l with the "C" locale, restricted to the grammar of std::from_chars(chars_format::general): strtod also
// takes leading whitespace, '+' and hex, which from_chars does not. Correctly rounded on glibc and Apple's libc, so
// it gives the same double as from_chars; tests/test_units.cpp checks that wherever from_chars exists.
inline std::optional<double> parse_double_strtod(std::string_view s) {
  if (s.empty()) return std::nullopt;
  const std::size_t sign = s[0] == '-' ? 1 : 0;
  if (sign == s.size()) return std::nullopt;
  const char c = s[sign];
  if (!((c >= '0' && c <= '9') || c == '.' || c == 'i' || c == 'I' || c == 'n' || c == 'N')) return std::nullopt;
  if (c == '0' && sign + 1 < s.size() && (s[sign + 1] == 'x' || s[sign + 1] == 'X')) return std::nullopt;
  static const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t{});
  const std::string buf(s);  // strtod needs a terminated string
  char* end = nullptr;
  errno = 0;
  const double v = strtod_l(buf.c_str(), &end, c_locale);
  if (end != buf.c_str() + buf.size()) return std::nullopt;
  // strtod also sets ERANGE for a subnormal result, which from_chars returns; only overflow and underflow to zero
  // are result_out_of_range there.
  if (errno == ERANGE && (v == 0.0 || v == HUGE_VAL || v == -HUGE_VAL)) return std::nullopt;
  return v;
}

#if !defined(__APPLE__)
inline std::optional<double> parse_double_from_chars(std::string_view s) {
  double v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}
#endif

}  // namespace detail

// Parses all of `s` as a decimal floating-point number, independent of the C locale; nullopt if it is not one
// or a character is left over. Apple's libc++ marks floating-point std::from_chars unavailable before macOS 26,
// so Apple platforms use the strtod_l path (D135).
inline std::optional<double> parse_double(std::string_view s) {
#if defined(__APPLE__)
  return detail::parse_double_strtod(s);
#else
  return detail::parse_double_from_chars(s);
#endif
}

}  // namespace tmk
