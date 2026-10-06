// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <map>

#include "crules/engine.hpp"

namespace tmk::crules {
namespace {

std::string mm3(Coord v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(v) / 1e6);
  return buf;
}

}  // namespace

std::vector<SyntheticClass> synthetic_net_classes(const model::Board& b, const model::DesignRules& rules, const Catalogue& cat,
                                                  const Detection& det, const Evaluation& ev) {
  std::vector<SyntheticClass> out;
  std::map<model::NetId, std::size_t> of_net;  // a net gets one class; a second rule can only widen it
  for (const auto& e : ev.rules) {
    if (!e.spec || e.instance < 0 || static_cast<std::size_t>(e.instance) >= det.instances.size()) continue;
    const Instance& in = det.instances[static_cast<std::size_t>(e.instance)];
    if (in.confidence < cat.apply || in.disabled(e.spec->id) || e.status == Status::SatisfiedByBoard) continue;
    Coord width = 0, pair_width = 0, pair_gap = 0;
    std::string how;
    if (e.spec->kind == "impedance" && e.impedance && e.impedance->computed) {
      const LineSolution* outer = nullptr;
      bool inner_differs = false;
      for (const auto& l : e.impedance->lines) {
        if (!l.ok) continue;
        if (!outer) outer = &l;  // lines are in copper order: the first solved layer is the front
        else if (l.width != outer->width || l.gap != outer->gap) inner_differs = true;
      }
      if (!outer) continue;
      width = outer->width;
      if (outer->differential) pair_width = outer->width, pair_gap = outer->gap;
      how = std::string(outer->differential ? "pair " : "") + mm3(outer->width) + (outer->differential ? " / gap " + mm3(outer->gap) : "") + " mm for " +
            std::to_string(static_cast<int>(outer->target)) + " ohm on " + outer->layer +
            (inner_differs ? "; other layers need a different width, which the router's single width per net does not give" : "");
    } else if (e.spec->kind == "width_for_current") {
      if (e.current && e.current->computed) {
        width = e.current->outer_width;
        how = mm3(width) + " mm for " + mm3(static_cast<Coord>(e.current->amps * 1e6)) + " A on outer layers" +
              (e.current->inner_width > e.current->outer_width ? "; inner layers would need " + mm3(e.current->inner_width) + " mm" : "") +
              (e.current->copper_assumed ? " (35 um copper assumed)" : "");
      } else if (e.spec->params.contains("min_width_mm") && e.spec->params["min_width_mm"].is_number()) {
        width = static_cast<Coord>(e.spec->params["min_width_mm"].get<double>() * 1e6);
        how = "minimum width " + mm3(width) + " mm";
      }
    }
    if (width <= 0) continue;
    std::vector<model::NetId> nets;
    for (const auto& role : e.spec->applies_to)
      if (const Role* r = in.role(role))
        for (model::NetId n : r->nets)
          if (n > 0 && static_cast<std::size_t>(n) < b.nets.size() && std::find(nets.begin(), nets.end(), n) == nets.end()) nets.push_back(n);
    // Board classes win: only nets still in Default.
    std::erase_if(nets, [&](model::NetId n) { return rules.class_for(b.nets[static_cast<std::size_t>(n)].name).name != rules.default_class().name; });
    if (nets.empty()) continue;
    const model::NetClass& base = rules.default_class();
    SyntheticClass sc;
    sc.cls = base;
    sc.cls.name = "tmk_" + e.spec->id + "_" + b.footprints[static_cast<std::size_t>(in.anchor)].reference;
    sc.cls.priority = 0;
    // Never looser than the board: at least the Default width and the board minimum.
    sc.cls.track_width = std::max({width, pair_width > 0 ? Coord{0} : base.track_width, rules.minimums.track_width});
    if (pair_width > 0) {
      sc.cls.track_width = std::max(pair_width, rules.minimums.track_width);
      sc.cls.diff_pair_width = sc.cls.track_width;
      sc.cls.diff_pair_gap = std::max(pair_gap, rules.minimums.clearance);
      sc.cls.has_diff_pair_gap = true;
    }
    sc.rule = e.spec->id;
    sc.detail = how;
    for (model::NetId n : nets) {
      if (const auto it = of_net.find(n); it != of_net.end()) {
        auto& prev = out[it->second];
        if (sc.cls.track_width > prev.cls.track_width) {
          prev.cls.track_width = sc.cls.track_width;
          prev.detail += "; widened by " + e.spec->id + " (" + how + ")";
        }
        continue;
      }
      sc.nets.push_back(n);
    }
    if (sc.nets.empty()) continue;
    for (model::NetId n : sc.nets) of_net[n] = out.size();
    out.push_back(std::move(sc));
  }
  return out;
}

model::DesignRules with_synthetic_classes(const model::Board& b, model::DesignRules rules, const std::vector<SyntheticClass>& sc) {
  for (const auto& c : sc) {
    rules.classes.push_back(c.cls);
    for (model::NetId n : c.nets) rules.assignments[b.nets[static_cast<std::size_t>(n)].name] = {c.cls.name};
  }
  return rules;
}

}  // namespace tmk::crules
