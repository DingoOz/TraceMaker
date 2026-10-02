#include "model/rules.hpp"

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
    if (wildcard_match(pattern, net_name))
      if (const NetClass* c = find_class(cls)) return *c;
  return default_class();
}

}  // namespace tmk::model
