// SPDX-License-Identifier: GPL-3.0-or-later
#include "model/rules.hpp"

#include <algorithm>
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

std::vector<std::string> DesignRules::classes_of(const std::string& net_name) const {
  std::vector<const NetClass*> found;
  auto add = [&](const std::string& cls) {
    if (const NetClass* c = find_class(cls); c && std::find(found.begin(), found.end(), c) == found.end()) found.push_back(c);
  };
  if (auto it = assignments.find(net_name); it != assignments.end())
    for (const auto& cls : it->second) add(cls);
  for (const auto& [pattern, cls] : patterns)
    if (pattern_match(pattern, net_name)) add(cls);
  // NET_SETTINGS::GetEffectiveNetClass (KiCad 10.0.6): no class is Default alone; otherwise Default joins when the classes leave a
  // parameter unset, or when none names a tuning profile and Default does (rule_parity static_has_class_default*).
  std::uint16_t fields = 0;
  bool profile = false;
  for (const NetClass* c : found) {
    fields |= c->fields;
    profile |= !c->tuning_profile.empty() && !c->profile_from_default;
  }
  const NetClass& def = default_class();
  if (found.empty() || fields != NetClass::kAllFields || (!profile && !def.tuning_profile.empty())) add(def.name);
  std::vector<std::string> out;
  for (const NetClass* c : found) out.push_back(c->name);
  return out;
}

}  // namespace tmk::model
