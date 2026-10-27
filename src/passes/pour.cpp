// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/pour.hpp"

#include <algorithm>
#include <stdexcept>

#include "crules/names.hpp"

namespace tmk::passes {

namespace {

constexpr Coord kMinPourClearance = 300'000;

model::Zone pour_zone(const model::Board& b, const model::DesignRules& rules, model::NetId net, int layer) {
  model::Zone z;
  z.net = net;
  z.copper = model::layer_bit(layer);
  z.layers = {b.copper_name(layer)};
  z.name = "tracemaker pour";
  const auto& name = b.nets[static_cast<std::size_t>(net)].name;
  const Coord cls = rules.classes.empty() ? 200'000 : rules.class_for(name).clearance;
  z.clearance = std::max(cls, kMinPourClearance);
  // Solid pad connections: the new copper of other nets cuts thermal spokes off the pour (KiCad's starved_thermal), 22 of
  // 33 failing PCBench boards in the M20 C3 run; a solid pad needs only to touch the fill (doc 05 §53).
  z.connect = model::ZoneConnect::Full;
  // A little beyond the outline's box (its arcs bulge past their end and mid points): the fill stops at the board edge anyway.
  const geom::Box box = b.edge_bbox().inflated(500'000);
  z.outline.push_back({{box.x0, box.y0}, {box.x1, box.y0}, {box.x1, box.y1}, {box.x0, box.y1}});
  return z;
}

}  // namespace

std::vector<PourPlan> plan_pours(const model::Board& b, const model::DesignRules& rules, const std::vector<std::string>& specs,
                                 std::vector<std::string>* notes) {
  auto note = [&](const std::string& s) {
    if (notes) notes->push_back(s);
  };
  const geom::Box box = b.edge_bbox();
  if (box.x1 <= box.x0 || box.y1 <= box.y0) {
    if (!specs.empty()) note("pour: the board has no outline on Edge.Cuts, so no pour is added");
    return {};
  }
  std::vector<PourPlan> out;
  auto taken = [&](model::NetId net, int layer) {
    return std::any_of(out.begin(), out.end(), [&](const PourPlan& p) { return p.zone.copper & model::layer_bit(layer) && (net == 0 || p.zone.net == net); });
  };
  for (const auto& spec : specs) {
    if (spec == "auto") {
      const bool has_zone = std::any_of(b.zones.begin(), b.zones.end(), [](const model::Zone& z) { return !z.rule_area && !z.teardrop && z.net > 0; });
      if (has_zone) {
        note("pour auto: the board has copper zones already, so none is added");
        continue;
      }
      std::vector<int> pads(b.nets.size(), 0);
      for (const auto& p : b.pads)
        if (p.net > 0) ++pads[static_cast<std::size_t>(p.net)];
      model::NetId gnd = 0;
      for (const auto& n : b.nets)
        if (n.id > 0 && pads[static_cast<std::size_t>(n.id)] > 0 && crules::ground_like_name(crules::net_leaf(n.name)) &&
            (gnd == 0 || pads[static_cast<std::size_t>(n.id)] > pads[static_cast<std::size_t>(gnd)]))
          gnd = n.id;
      if (gnd == 0) {
        note("pour auto: no ground net (by name) with pads, so no pour is added");
        continue;
      }
      const int layer = b.copper_count() > 2 ? 1 : b.copper_count() - 1;
      if (taken(0, layer)) continue;
      out.push_back({pour_zone(b, rules, gnd, layer), "auto: ground net " + b.nets[static_cast<std::size_t>(gnd)].name + " on " + b.copper_name(layer) +
                                                          (layer == 1 && b.copper_count() > 2 ? " (first inner layer)" : " (back layer)")});
      continue;
    }
    const auto at = spec.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == spec.size()) throw std::invalid_argument("--pour: expected auto or NET@LAYER, got '" + spec + "'");
    const std::string net_name = spec.substr(0, at), layer_name = spec.substr(at + 1);
    const model::NetId net = b.net_by_name(net_name);
    if (net == 0) throw std::invalid_argument("--pour: no net named '" + net_name + "'");
    const int layer = b.copper_index(layer_name);
    if (layer < 0) throw std::invalid_argument("--pour: '" + layer_name + "' is not a copper layer of this board");
    if (taken(0, layer)) throw std::invalid_argument("--pour: two pours on " + layer_name);
    out.push_back({pour_zone(b, rules, net, layer), "named: " + net_name + " on " + b.copper_name(layer)});
  }
  return out;
}

}  // namespace tmk::passes
