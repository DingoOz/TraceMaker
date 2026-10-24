// SPDX-License-Identifier: GPL-3.0-or-later
// The run report's own sections (doc 18 D3): rules read and not read, DRC before and after.
#include "report/run_report.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include "drc/drc.hpp"
#include "drc/refill.hpp"

namespace tmk::report {
namespace {

double mm3(Coord v) { return std::round(nm_to_mm(v) * 1000.0) / 1000.0; }
constexpr std::size_t kMaxMissing = 300;  // unconnected items listed after the route (the rest are counted)

}  // namespace

bool routing_violation(const std::string& type) {
  // bench/drc_judge.py NOT_ROUTING (D106), unconnected_items excepted: the report counts those separately.
  static const std::set<std::string> kNot = {
      "lib_footprint_issues", "lib_footprint_mismatch", "silk_overlap", "silk_over_copper", "silk_edge_clearance", "text_height",
      "text_thickness", "courtyards_overlap", "missing_courtyard", "malformed_courtyard", "footprint_type_mismatch",
      "footprint_filters_mismatch", "nonmirrored_text_on_back_layer", "npth_inside_courtyard", "pth_inside_courtyard", "duplicate_footprints",
      "extra_footprint", "missing_footprint", "footprint_symbol_mismatch", "track_dangling", "via_dangling", "holes_co_located"};
  return type != "unconnected_items" && !kNot.count(type);
}

nlohmann::json rules_json(const std::string& board_path, const model::Board& board, const model::DesignRules& rules,
                          const std::vector<std::string>& engine_warnings) {
  namespace fs = std::filesystem;
  const fs::path b(board_path);
  const fs::path pro = fs::path(b).replace_extension(".kicad_pro");
  const fs::path dru = fs::path(b).replace_extension(".kicad_dru");
  nlohmann::json j;
  j["files"] = {{"project", pro.filename().string()}, {"project_found", fs::exists(pro)},
                {"custom_rules", dru.filename().string()}, {"custom_rules_found", fs::exists(dru)}};
  const auto& m = rules.minimums;
  j["minimums"] = {{"clearance_mm", mm3(m.clearance)},
                   {"track_width_mm", mm3(m.track_width)},
                   {"via_diameter_mm", mm3(m.via_diameter)},
                   {"via_annular_width_mm", mm3(m.via_annular_width)},
                   {"drill_mm", mm3(m.through_hole_diameter)},
                   {"hole_clearance_mm", mm3(m.hole_clearance)},
                   {"hole_to_hole_mm", mm3(m.hole_to_hole)},
                   {"copper_edge_clearance_mm", mm3(m.copper_edge_clearance)},
                   {"blind_buried_vias", m.allow_blind_buried_vias},
                   {"microvias", m.allow_microvias}};
  // Nets per class, as the router resolves them (explicit assignment, then the first matching pattern, else Default).
  std::map<std::string, int> nets_in;
  for (const auto& n : board.nets)
    if (n.id > 0 && !n.name.empty()) ++nets_in[rules.class_for(n.name).name];
  nlohmann::json classes = nlohmann::json::array();
  for (const auto& c : rules.classes)
    classes.push_back({{"name", c.name},
                       {"nets", nets_in.count(c.name) ? nets_in[c.name] : 0},
                       {"clearance_mm", mm3(c.clearance)},
                       {"track_width_mm", mm3(c.track_width)},
                       {"via_diameter_mm", mm3(c.via_diameter)},
                       {"via_drill_mm", mm3(c.via_drill)},
                       {"diff_pair_width_mm", mm3(c.diff_pair_width)},
                       {"diff_pair_gap_mm", mm3(c.diff_pair_gap)}});
  j["net_classes"] = classes;
  nlohmann::json custom = nlohmann::json::array();
  for (const auto& r : rules.custom) {
    nlohmann::json types = nlohmann::json::array();
    for (const auto& c : r.constraints) types.push_back(c.type);
    custom.push_back({{"name", r.name}, {"layer", r.layer}, {"condition", r.condition}, {"constraints", types}});
  }
  j["custom_rules"] = custom;
  j["custom_rule_count"] = rules.custom.size();
  // Everything that was not read, each with where it came from. Engine warnings repeat project warnings now and then;
  // a line is listed once.
  nlohmann::json not_read = nlohmann::json::array();
  std::set<std::string> seen;
  auto add = [&](const char* source, const std::string& text) {
    if (seen.insert(text).second) not_read.push_back({{"source", source}, {"text", text}});
  };
  for (const auto& w : rules.warnings) add("project", w);
  for (const auto& w : engine_warnings) add("custom rules", w);
  for (const auto& w : board.warnings) add("board", w);
  j["not_read"] = not_read;
  j["not_read_count"] = not_read.size();
  return j;
}

nlohmann::json drc_json(const model::Board& before, const model::Board& after, const model::DesignRules& rules, bool refill) {
  struct Side {
    std::map<std::string, std::pair<int, std::string>> by_type;  // type -> (count, severity of the first)
    int errors = 0, warnings = 0, unconnected = 0;
    std::vector<std::string> notes;
    nlohmann::json missing = nlohmann::json::array();  // the unconnected items: what is still to be joined, and where
  };
  auto check = [&](const model::Board& b) {
    Side s;
    drc::DrcReport rep;
    if (refill) {
      auto rf = drc::refill_zones(b, rules);
      s.notes = rf.warnings;
      rep = drc::run_drc(rf.board, rules);
    } else {
      rep = drc::run_drc(b, rules);
    }
    for (const auto& v : rep.violations) {
      auto& e = s.by_type[v.type];
      if (e.first++ == 0) e.second = v.severity;
      (v.severity == "error" ? s.errors : s.warnings) += 1;
    }
    s.unconnected = static_cast<int>(rep.unconnected.size());
    for (const auto& u : rep.unconnected) {
      if (s.missing.size() >= kMaxMissing) break;
      nlohmann::json e{{"what", u.description}};
      if (u.items.size() >= 2)
        e.update({{"from", u.items[0].description}, {"to", u.items[1].description}, {"x_mm", mm3(u.items[0].pos.x)}, {"y_mm", mm3(u.items[0].pos.y)}});
      s.missing.push_back(std::move(e));
    }
    s.notes.insert(s.notes.end(), rep.warnings.begin(), rep.warnings.end());
    return s;
  };
  const Side b = check(before), a = check(after);
  std::set<std::string> types;
  for (const auto& [t, _] : b.by_type) types.insert(t);
  for (const auto& [t, _] : a.by_type) types.insert(t);
  nlohmann::json rows = nlohmann::json::array();
  int added_routing = 0, removed = 0;
  for (const auto& t : types) {
    const auto ib = b.by_type.find(t), ia = a.by_type.find(t);
    const int nb = ib == b.by_type.end() ? 0 : ib->second.first;
    const int na = ia == a.by_type.end() ? 0 : ia->second.first;
    const std::string sev = ia != a.by_type.end() ? ia->second.second : ib->second.second;
    const bool routing = routing_violation(t);
    if (routing && sev == "error") added_routing += std::max(na - nb, 0);
    removed += std::max(nb - na, 0);
    rows.push_back({{"type", t}, {"severity", sev}, {"routing", routing}, {"before", nb}, {"after", na}, {"added", std::max(na - nb, 0)}});
  }
  auto side = [](const Side& s) {
    return nlohmann::json{{"errors", s.errors}, {"warnings", s.warnings}, {"unconnected", s.unconnected}, {"notes", s.notes}};
  };
  auto after_side = side(a);
  // Only after the route: before it, every connection is unconnected and the list says nothing.
  after_side["unconnected_items"] = a.missing;
  if (a.unconnected > static_cast<int>(a.missing.size())) after_side["unconnected_items_not_shown"] = a.unconnected - static_cast<int>(a.missing.size());
  return {{"checker", "tracemaker drc"},
          {"zones_refilled", refill},
          {"before", side(b)},
          {"after", after_side},
          {"types", rows},
          {"added_routing_errors", added_routing},
          {"removed", removed},
          {"unconnected_change", a.unconnected - b.unconnected}};
}

}  // namespace tmk::report
