// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/fit_rules.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "drc/drc.hpp"

namespace tmk::drc {

namespace {

// The measured values of one violation type (errors only; violations without a measurement are left out).
struct Seen {
  std::vector<Coord> values;
  Coord fit = -1;
  int count = 0;
};

void lower(Coord& value, Coord to, const char* rule, int evidence, std::vector<FittedRule>& out, std::string note = {}) {
  if (to >= value) return;
  out.push_back({rule, value, to, evidence, std::move(note)});
  value = to;
}

}  // namespace

FitReport fit_rules(const model::Board& ref, model::DesignRules& rules, const FitOptions& opt) {
  FitReport rep;
  DrcOptions d;
  d.dangling = false;
  const auto drc = run_drc(ref, rules, d);
  std::map<std::string, Seen> seen;
  for (const auto& v : drc.violations)
    if (v.severity == "error" && v.actual >= 0) seen[v.type].values.push_back(v.actual);
  // The value a rule is fitted to: the smallest measured, or for the clearance the low decile. A clearance has thousands of pairs behind
  // it and a few slips among them (one pair at 0.01 mm on sizif512ext against a 0.2 mm rule); edge and hole gaps come from where a
  // connector or a hole sits and are what they are. Fewer than `min_evidence` violations are taken for slips and fit nothing.
  auto fitted = [&](const char* type, Coord floor, double quantile = 0.0) -> const Seen* {
    const auto it = seen.find(type);
    if (it == seen.end()) return nullptr;
    auto& s = it->second;
    s.count = static_cast<int>(s.values.size());
    rep.violations += s.count;
    if (s.count < opt.min_evidence) return nullptr;
    std::sort(s.values.begin(), s.values.end());
    s.fit = std::max(s.values[static_cast<std::size_t>(quantile * static_cast<double>(s.count - 1))], floor);
    return &s;
  };
  auto& m = rules.minimums;
  if (const Seen* s = fitted("copper_edge_clearance", opt.edge_floor)) lower(m.copper_edge_clearance, s->fit, "copper_edge_clearance", s->count, rep.changes);
  if (const Seen* s = fitted("hole_clearance", opt.hole_floor)) lower(m.hole_clearance, s->fit, "hole_clearance", s->count, rep.changes);
  if (const Seen* s = fitted("clearance", opt.clearance_floor, opt.clearance_quantile)) {
    lower(m.clearance, s->fit, "clearance", s->count, rep.changes);
    // Clearance is a class value: every class above the fitted value comes down to it. The violation does not say which classes its two
    // items are in, and a clearance holds between two classes at the larger of theirs, so one class alone cannot be fitted.
    for (auto& c : rules.classes) lower(c.clearance, s->fit, ("class " + c.name + " clearance").c_str(), s->count, rep.changes);
  }
  if (const Seen* s = fitted("track_width", opt.width_floor)) lower(m.track_width, s->fit, "track_width", s->count, rep.changes);
  if (const Seen* s = fitted("annular_width", opt.annular_floor)) lower(m.via_annular_width, s->fit, "via_annular_width", s->count, rep.changes);
  if (const Seen* s = fitted("via_diameter", opt.via_floor)) lower(m.via_diameter, s->fit, "via_diameter", s->count, rep.changes);
  if (const Seen* s = fitted("drill_out_of_range", opt.drill_floor)) lower(m.through_hole_diameter, s->fit, "through_hole_diameter", s->count, rep.changes);

  // Class values the reference's own tracks and vias undercut: the width that carries half of the class's track length or more (the
  // length-weighted median), and its most used via. A class whose tracks are as wide as it asks, or wider, is left alone.
  std::map<std::string, std::map<Coord, double>> widths;                 // class -> width -> length
  std::map<std::string, std::map<std::pair<Coord, Coord>, int>> vias;    // class -> (size, drill) -> count
  auto class_of = [&](model::NetId n) -> std::string {
    if (n <= 0 || static_cast<std::size_t>(n) >= ref.nets.size()) return {};
    return rules.class_for(ref.nets[static_cast<std::size_t>(n)].name).name;
  };
  for (const auto& t : ref.tracks)
    if (const auto c = class_of(t.net); !c.empty()) widths[c][t.width] += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
  for (const auto& v : ref.vias)
    if (const auto c = class_of(v.net); !c.empty() && v.type == model::ViaType::Through) ++vias[c][{v.size, v.drill}];
  for (auto& c : rules.classes) {
    if (const auto it = widths.find(c.name); it != widths.end()) {
      double total = 0, acc = 0;
      const int n = static_cast<int>(it->second.size());  // distinct widths
      for (const auto& [w, len] : it->second) total += len;
      Coord median = 0;
      for (const auto& [w, len] : it->second) {  // ascending widths
        acc += len;
        if (acc * 2 >= total) {
          median = w;
          break;
        }
      }
      if (median > 0) lower(c.track_width, std::max({median, opt.width_floor, m.track_width}), ("class " + c.name + " track_width").c_str(), n, rep.changes, "widths among the reference's tracks of this class; the one carrying half their length");
    }
    if (const auto it = vias.find(c.name); it != vias.end()) {
      std::pair<Coord, Coord> best{};
      int most = 0, n = 0;
      for (const auto& [sd, k] : it->second) {  // ascending (size, drill): ties go to the smaller via
        n += k;
        if (k > most) most = k, best = sd;
      }
      // Taken only as a whole and only when legal under the (fitted) minimums: a via's size and drill belong together.
      const bool legal = best.first >= std::max(m.via_diameter, opt.via_floor) && best.second >= std::max(m.through_hole_diameter, opt.drill_floor) &&
                         (best.first - best.second) / 2 >= m.via_annular_width;
      if (most > 0 && legal && best.first < c.via_diameter) {
        rep.changes.push_back({"class " + c.name + " via_diameter", c.via_diameter, best.first, n, "vias of this class on the reference; its most used"});
        rep.changes.push_back({"class " + c.name + " via_drill", c.via_drill, best.second, n, "vias of this class on the reference; its most used"});
        c.via_diameter = best.first;
        c.via_drill = best.second;
      }
    }
  }
  return rep;
}

}  // namespace tmk::drc
