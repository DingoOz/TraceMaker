// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/inspect.hpp"

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace tmk::app {
using nlohmann::json;

void print_summary(const model::Board& b, const std::string& path) {
  std::size_t named = 0;
  for (const auto& n : b.nets) named += !n.name.empty();
  const auto bb = b.edge_bbox();
  std::printf("%s\n  format %lld (%s nets), %d copper layers:", path.c_str(), static_cast<long long>(b.version),
              b.named_nets ? "named" : "numbered", b.copper_count());
  for (int i = 0; i < b.copper_count(); ++i) std::printf(" %s", b.copper_name(i).c_str());
  std::printf("\n  %zu nets, %zu footprints, %zu pads, %zu tracks, %zu arcs, %zu vias, %zu zones\n", named,
              b.footprints.size(), b.pads.size(), b.tracks.size(), b.arcs.size(), b.vias.size(), b.zones.size());
  if (!bb.empty())
    std::printf("  outline %.3f x %.3f mm\n", nm_to_mm(bb.x1 - bb.x0), nm_to_mm(bb.y1 - bb.y0));
  for (const auto& w : b.warnings) std::printf("  warning: %s\n", w.c_str());
}

void write_truth_json(const model::Board& b, const std::string& out_path, const model::DesignRules* rules) {
  auto net = [&](model::NetId n) { return b.nets[static_cast<std::size_t>(n)].name; };
  json d;
  json cu = json::array();
  for (int i = 0; i < b.copper_count(); ++i) cu.push_back(b.copper_name(i));
  d["copper_layers"] = cu;
  std::vector<std::string> nets;
  for (const auto& n : b.nets)
    if (!n.name.empty()) nets.push_back(n.name);
  std::sort(nets.begin(), nets.end());
  d["nets"] = nets;
  if (rules) {  // design widths for quality metrics (narrowed tracks); not part of the KiCad truth schema
    json w = json::object();
    for (const auto& n : nets) w[n] = std::max(rules->class_for(n).track_width, rules->minimums.track_width);
    d["net_track_width"] = w;
    d["min_track_width"] = rules->minimums.track_width;
  }
  d["footprints"] = json::array();
  d["pads"] = json::array();
  for (const auto& f : b.footprints)
    d["footprints"].push_back({{"ref", f.reference}, {"x", f.pos.x}, {"y", f.pos.y}, {"angle", f.angle},
                               {"back", f.back}, {"locked", f.locked}, {"pads", f.pads.size()}});
  static const char* kAttr[] = {"smd", "thru_hole", "np_thru_hole", "connect"};
  for (const auto& p : b.pads) {
    json layers = json::array();
    for (int i = 0; i < b.copper_count(); ++i)
      if (p.copper & model::layer_bit(i)) layers.push_back(b.copper_name(i));
    d["pads"].push_back({{"ref", b.footprints[static_cast<std::size_t>(p.footprint)].reference}, {"num", p.number},
                         {"x", p.pos.x}, {"y", p.pos.y}, {"angle", geom::norm_deg(p.angle)}, {"w", p.size_x},
                         {"h", p.size_y}, {"drill_w", p.drill_x}, {"drill_h", p.drill_y},
                         {"attr", kAttr[static_cast<int>(p.type)]}, {"net", net(p.net)}, {"layers", layers}});
  }
  d["tracks"] = json::array();
  for (const auto& t : b.tracks)
    d["tracks"].push_back({{"sx", t.a.x}, {"sy", t.a.y}, {"ex", t.b.x}, {"ey", t.b.y}, {"width", t.width},
                           {"layer", b.copper_name(t.layer)}, {"net", net(t.net)}});
  d["arcs"] = json::array();
  for (const auto& t : b.arcs)
    d["arcs"].push_back({{"sx", t.a.x}, {"sy", t.a.y}, {"mx", t.mid.x}, {"my", t.mid.y}, {"ex", t.b.x}, {"ey", t.b.y},
                         {"width", t.width}, {"layer", b.copper_name(t.layer)}, {"net", net(t.net)}});
  d["vias"] = json::array();
  static const char* kVia[] = {"through", "blind", "micro"};
  for (const auto& v : b.vias)
    d["vias"].push_back({{"x", v.pos.x}, {"y", v.pos.y}, {"size", v.size}, {"drill", v.drill},
                         {"top", b.copper_name(v.layer_top)}, {"bottom", b.copper_name(v.layer_bottom)},
                         {"type", kVia[static_cast<int>(v.type)]}, {"net", net(v.net)}});
  d["zones"] = json::array();
  for (const auto& z : b.zones)
    if (z.footprint < 0) d["zones"].push_back({{"net", net(z.net)}, {"rule_area", z.rule_area}, {"layers", z.layers}});
  std::ofstream(out_path) << d.dump();
}

}  // namespace tmk::app
