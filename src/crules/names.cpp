#include "crules/names.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace tmk::crules {

namespace {
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
}  // namespace

std::string upper(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

std::string ref_letters(std::string_view ref) {
  std::string r;
  for (char c : ref) {
    if (!is_alpha(c)) break;
    r += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return r;
}

bool natural_less(std::string_view a, std::string_view b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (is_digit(a[i]) && is_digit(b[j])) {
      std::size_t i2 = i, j2 = j;
      while (i2 < a.size() && is_digit(a[i2])) ++i2;
      while (j2 < b.size() && is_digit(b[j2])) ++j2;
      // Compare digit runs as numbers without overflow: strip leading zeros, then length, then text.
      std::size_t ia = i, jb = j;
      while (ia + 1 < i2 && a[ia] == '0') ++ia;
      while (jb + 1 < j2 && b[jb] == '0') ++jb;
      const std::string_view na = a.substr(ia, i2 - ia), nb = b.substr(jb, j2 - jb);
      if (na.size() != nb.size()) return na.size() < nb.size();
      if (na != nb) return na < nb;
      i = i2;
      j = j2;
      continue;
    }
    const char ca = static_cast<char>(std::toupper(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(std::toupper(static_cast<unsigned char>(b[j])));
    if (ca != cb) return ca < cb;
    ++i;
    ++j;
  }
  if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
  return a < b;
}

std::string_view net_leaf(std::string_view name) {
  const auto s = name.rfind('/');
  return s == std::string_view::npos ? name : name.substr(s + 1);
}

bool power_like_name(std::string_view name) {
  const std::string n = upper(net_leaf(name));
  if (n.empty()) return false;
  if (n.find("GND") != std::string::npos || n.find("PWR") != std::string::npos) return true;
  if ((n[0] == '+' || n[0] == '-') && n.size() > 1 && (is_digit(n[1]) || n[1] == 'V')) return true;
  for (const char* pfx : {"VCC", "VDD", "VSS", "VEE", "VBUS", "VBAT", "VIN", "VSYS", "VPP", "VMOT", "V+", "V-", "AVCC", "AVDD", "DVDD", "VDDA", "VDDIO"})
    if (n.starts_with(pfx)) return true;
  // "3V3", "5V", "12V", "1V8": digits, a V, optional digits.
  std::size_t i = 0;
  while (i < n.size() && is_digit(n[i])) ++i;
  if (i > 0 && i < n.size() && n[i] == 'V') {
    std::size_t j = i + 1;
    while (j < n.size() && is_digit(n[j])) ++j;
    if (j == n.size()) return true;
  }
  return false;
}

bool ground_like_name(std::string_view name) {
  const std::string n = upper(net_leaf(name));
  return n.find("GND") != std::string::npos || n.starts_with("VSS") || n == "0V";
}

bool analog_supply_name(std::string_view name) {
  const std::string n = upper(net_leaf(name));
  for (const char* pfx : {"VREF", "AREF", "VCAP", "AVCC", "AVDD", "VDDA", "VLCD", "VIO", "VDDIO", "VDDUSB", "VUSB", "V3V3", "V5V", "VCORE"})
    if (n.starts_with(pfx)) return true;
  return n == "REF" || n.ends_with("_VREF") || n.ends_with("_AREF");
}

std::optional<int> net_millivolts(std::string_view name) {
  static const std::regex re(R"((\d+)(?:[.,](\d+))?V(\d*))", std::regex::ECMAScript | std::regex::optimize);
  const std::string n = upper(net_leaf(name));
  std::smatch m;
  if (!std::regex_search(n, m, re)) return std::nullopt;
  const std::string whole = m[1].str();
  std::string frac = m[2].matched ? m[2].str() : m[3].str();
  if (whole.size() > 4) return std::nullopt;
  if (frac.size() > 3) frac.resize(3);
  while (frac.size() < 3) frac += '0';
  return std::stoi(whole) * 1000 + std::stoi(frac);
}

std::optional<std::int64_t> capacitance_pf(std::string_view value) {
  std::string v = lower(value);
  for (std::size_t p; (p = v.find("\xc2\xb5")) != std::string::npos;) v.replace(p, 2, "u");  // micro sign
  static const std::regex re(R"(^\s*(\d+)(?:[.,](\d+))?\s*([pnum])(\d*))", std::regex::ECMAScript | std::regex::optimize);
  std::smatch m;
  if (!std::regex_search(v, m, re)) return std::nullopt;
  const std::string whole = m[1].str();
  const std::string frac = m[2].matched ? m[2].str() : m[4].str();
  if (whole.size() > 6 || frac.size() > 6) return std::nullopt;
  std::int64_t unit = 1;
  switch (m[3].str()[0]) {
    case 'n': unit = 1'000; break;
    case 'u': unit = 1'000'000; break;
    case 'm': unit = 1'000'000'000; break;
    default: break;
  }
  std::int64_t scale = 1;
  for (std::size_t i = 0; i < frac.size(); ++i) scale *= 10;
  const std::int64_t mant = std::stoll(whole) * scale + (frac.empty() ? 0 : std::stoll(frac));
  return mant * unit / scale;
}

}  // namespace tmk::crules
