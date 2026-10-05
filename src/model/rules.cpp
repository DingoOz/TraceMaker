// SPDX-License-Identifier: GPL-3.0-or-later
#include "model/rules.hpp"

#include <regex>

namespace tmk::model {

bool wildcard_match(std::string_view p, std::string_view t) {
  std::size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
  while (ti < t.size()) {
    if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
      ++pi;
      ++ti;
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ti;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      ti = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

bool pattern_match(const std::string& pattern, const std::string& text) {
  // KiCad's combined matcher: a wildcard match, or a full regular-expression match.
  if (wildcard_match(pattern, text)) return true;
  static thread_local std::map<std::string, std::optional<std::regex>> cache;
  auto it = cache.find(pattern);
  if (it == cache.end()) {
    std::optional<std::regex> re;
    try {
      re.emplace(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error&) {
    }
    it = cache.emplace(pattern, std::move(re)).first;
  }
  return it->second && std::regex_match(text, *it->second);
}

const NetClass* DesignRules::find_class(const std::string& name) const {
  for (const auto& c : classes)
    if (c.name == name) return &c;
  return nullptr;
}

const NetClass& DesignRules::class_for(const std::string& net_name) const {
  if (auto it = assignments.find(net_name); it != assignments.end())
    for (const auto& cls : it->second)
      if (const NetClass* c = find_class(cls)) return *c;
  for (const auto& [pattern, cls] : patterns)
    if (pattern_match(pattern, net_name))
      if (const NetClass* c = find_class(cls)) return *c;
  return default_class();
}

}  // namespace tmk::model
