// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/kicad/project_reader.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sexpr/sexpr.hpp"

namespace tmk::io {
namespace fs = std::filesystem;
using nlohmann::json;

std::optional<Coord> parse_length(std::string_view s) {
  auto strip = [&](std::string_view suf) {
    if (s.size() > suf.size() && s.substr(s.size() - suf.size()) == suf) {
      s.remove_suffix(suf.size());
      return true;
    }
    return false;
  };
  double scale = 1.0;  // to mm
  if (strip("mm")) scale = 1.0;
  else if (strip("mil")) scale = 0.0254;
  else if (strip("um")) scale = 0.001;
  else if (strip("in")) scale = 25.4;
  if (scale == 1.0) return sexpr::parse_mm(s);
  double v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  const double nm = v * scale * 1e6;
  return static_cast<Coord>(nm < 0 ? nm - 0.5 : nm + 0.5);
}

namespace {

Coord mm(const json& j, const char* key, Coord def) {
  if (!j.contains(key) || j[key].is_null() || !j[key].is_number()) return def;
  const double v = j[key].get<double>() * 1e6;
  return static_cast<Coord>(v < 0 ? v - 0.5 : v + 0.5);
}

struct LegacyRules {
  bool design_settings = false;
  bool netclasses = false;
};

void read_pro(const fs::path& p, model::DesignRules& r, const LegacyRules& legacy) {
  std::ifstream f(p);
  json d;
  try {
    d = json::parse(f);
  } catch (const json::exception& e) {
    r.warnings.push_back("cannot parse " + p.string() + ": " + e.what());
    return;
  }
  if (!legacy.design_settings && d.contains("board") && d["board"].contains("design_settings") && d["board"]["design_settings"].contains("rules")) {
    const json& j = d["board"]["design_settings"]["rules"];
    auto& m = r.minimums;
    m.clearance = mm(j, "min_clearance", m.clearance);
    m.track_width = mm(j, "min_track_width", m.track_width);
    m.connection_width = mm(j, "min_connection", m.connection_width);
    m.via_diameter = mm(j, "min_via_diameter", m.via_diameter);
    m.via_annular_width = mm(j, "min_via_annular_width", m.via_annular_width);
    m.through_hole_diameter = mm(j, "min_through_hole_diameter", m.through_hole_diameter);
    m.microvia_diameter = mm(j, "min_microvia_diameter", m.microvia_diameter);
    m.microvia_drill = mm(j, "min_microvia_drill", m.microvia_drill);
    m.hole_clearance = mm(j, "min_hole_clearance", m.hole_clearance);
    m.hole_to_hole = mm(j, "min_hole_to_hole", m.hole_to_hole);
    m.copper_edge_clearance = mm(j, "min_copper_edge_clearance", m.copper_edge_clearance);
    m.silk_clearance = mm(j, "min_silk_clearance", m.silk_clearance);
    m.solder_mask_to_copper_clearance = mm(j, "solder_mask_to_copper_clearance", m.solder_mask_to_copper_clearance);
    m.allow_blind_buried_vias = j.value("allow_blind_buried_vias", m.allow_blind_buried_vias);
    m.allow_microvias = j.value("allow_microvias", m.allow_microvias);
    if (j.contains("use_height_for_length_calcs") && j["use_height_for_length_calcs"].is_boolean())
      r.use_height_for_length = j["use_height_for_length_calcs"].get<bool>();
    const json& ds = d["board"]["design_settings"];
    if (ds.contains("rule_severities") && ds["rule_severities"].is_object())
      for (const auto& [type, sev] : ds["rule_severities"].items())
        if (sev.is_string()) r.severities[type] = sev.get<std::string>();
  } else if (!legacy.design_settings) {
    r.warnings.push_back("project has no board design rules; using KiCad defaults and rules stored in the board");
  }
  if (legacy.netclasses || !d.contains("net_settings")) return;
  const json& ns = d["net_settings"];
  if (ns.contains("classes") && ns["classes"].is_array()) {
    // Default first; other classes inherit unset (null) values from Default.
    model::NetClass def;
    def.track_width = 200'000;  // KiCad 10 common/netclass.cpp DEFAULT_TRACK_WIDTH
    def.name = "Default";
    def.priority = INT_MAX;
    for (const auto& c : ns["classes"])
      if (c.value("name", "") == "Default") {
        def.clearance = mm(c, "clearance", def.clearance);
        def.track_width = mm(c, "track_width", def.track_width);
        def.via_diameter = mm(c, "via_diameter", def.via_diameter);
        def.via_drill = mm(c, "via_drill", def.via_drill);
        def.uvia_diameter = mm(c, "microvia_diameter", def.uvia_diameter);
        def.uvia_drill = mm(c, "microvia_drill", def.uvia_drill);
        def.diff_pair_width = mm(c, "diff_pair_width", def.diff_pair_width);
        def.diff_pair_gap = mm(c, "diff_pair_gap", def.diff_pair_gap);
        def.diff_pair_via_gap = mm(c, "diff_pair_via_gap", def.diff_pair_via_gap);
        def.has_diff_pair_gap = c.contains("diff_pair_gap") && c["diff_pair_gap"].is_number();
      }
    r.classes.clear();
    r.classes.push_back(def);
    for (const auto& c : ns["classes"]) {
      const std::string name = c.value("name", "");
      if (name == "Default" || name.empty()) continue;
      model::NetClass k = def;
      k.name = name;
      k.priority = c.contains("priority") && c["priority"].is_number() ? c["priority"].get<int>() : 0;
      k.clearance = mm(c, "clearance", def.clearance);
      k.track_width = mm(c, "track_width", def.track_width);
      k.via_diameter = mm(c, "via_diameter", def.via_diameter);
      k.via_drill = mm(c, "via_drill", def.via_drill);
      k.uvia_diameter = mm(c, "microvia_diameter", def.uvia_diameter);
      k.uvia_drill = mm(c, "microvia_drill", def.uvia_drill);
      k.diff_pair_width = mm(c, "diff_pair_width", def.diff_pair_width);
      k.diff_pair_gap = mm(c, "diff_pair_gap", def.diff_pair_gap);
      k.diff_pair_via_gap = mm(c, "diff_pair_via_gap", def.diff_pair_via_gap);
      k.has_diff_pair_gap = c.contains("diff_pair_gap") && c["diff_pair_gap"].is_number();
      r.classes.push_back(k);
    }
  }
  if (ns.contains("netclass_patterns") && ns["netclass_patterns"].is_array())
    for (const auto& pat : ns["netclass_patterns"]) r.patterns.emplace_back(pat.value("pattern", ""), pat.value("netclass", ""));
  if (ns.contains("netclass_assignments") && ns["netclass_assignments"].is_object())
    for (const auto& [net, v] : ns["netclass_assignments"].items()) {
      std::vector<std::string> cls;
      if (v.is_string()) cls.push_back(v.get<std::string>());
      else if (v.is_array())
        for (const auto& x : v)
          if (x.is_string()) cls.push_back(x.get<std::string>());
      r.assignments[net] = cls;
    }
}

void read_dru(const fs::path& p, model::DesignRules& r) {
  std::ifstream f(p, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  // KiCad rule files allow '#' line comments; blank them out (keeping offsets) before parsing.
  bool in_str = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) in_str = !in_str;
    if (!in_str && text[i] == '#')
      while (i < text.size() && text[i] != '\n') text[i++] = ' ';
  }
  // The file is a sequence of top-level expressions; wrap it so the parser sees one document.
  sexpr::Document d;
  try {
    d = sexpr::Document::parse("(rules\n" + text + "\n)");
  } catch (const sexpr::ParseError& e) {
    r.warnings.push_back("cannot parse " + p.string() + ": " + e.what());
    return;
  }
  for (sexpr::NodeId rule : d.find_all(d.root(), "rule")) {
    model::CustomRule cr;
    cr.name = d.str_at(rule, 1);
    if (auto c = d.find(rule, "condition"); c != sexpr::kNoNode) cr.condition = d.str_at(c, 1);
    if (auto l = d.find(rule, "layer"); l != sexpr::kNoNode) cr.layer = d.str_at(l, 1);
    if (auto s = d.find(rule, "severity"); s != sexpr::kNoNode) cr.severity = d.str_at(s, 1);
    for (sexpr::NodeId c : d.find_all(rule, "constraint")) {
      model::Constraint k;
      k.type = d.str_at(c, 1);
      for (std::size_t i = 2; i < d.children(c).size(); ++i) {
        const sexpr::NodeId x = d.child(c, i);
        if (!d.is_list(x)) {
          k.items.push_back(d.str(x));
          continue;
        }
        const std::string_view h = d.head(x);
        // KiCad's only flag in brackets: the skew applies between the halves of each pair, not across the rule's
        // nets. Anything else without a value stays unread and is warned about below (rule 6).
        if (d.children(x).size() == 1 && h == "within_diff_pairs" && k.type == "skew") {
          k.items.push_back(std::string(h));
          continue;
        }
        const auto v = parse_length(d.str_at(x, 1));
        if (!v) {
          r.warnings.push_back("rule '" + cr.name + "': cannot read value of " + std::string(h));
          continue;
        }
        if (h == "min") k.min = v;
        else if (h == "opt") k.opt = v;
        else if (h == "max") k.max = v;
      }
      cr.constraints.push_back(std::move(k));
    }
    r.custom.push_back(std::move(cr));
  }
}

// KiCad 10 include/board_design_settings.h DEFAULT_* and BOARD_DESIGN_SETTINGS constructor.
// Missing project keys retain constructor or legacy parser values (m_resetParamsIfMissing = false).
void kicad_defaults(model::DesignRules& r) {
  auto& m = r.minimums;
  m.clearance = 0;
  m.track_width = 200'000;
  m.via_diameter = 500'000;
  m.through_hole_diameter = 300'000;
  m.via_annular_width = 100'000;
  m.microvia_diameter = 200'000;
  m.microvia_drill = 100'000;
  m.hole_clearance = 250'000;
  m.hole_to_hole = 250'000;
  m.copper_edge_clearance = 500'000;
}

// KiCad <= 5 boards carry rules in the board file: (setup (trace_min) (via_min_size) (via_min_drill) ...) and
// (net_class name "descr" (clearance) (trace_width) (via_dia) (via_drill) ... (add_net "x")).
LegacyRules read_legacy_board_rules(const fs::path& board, model::DesignRules& r) {
  sexpr::Document d;
  try {
    d = sexpr::Document::load(board.string());
  } catch (const std::exception&) {
    return {};
  }
  LegacyRules legacy;
  auto nm = [&](sexpr::NodeId parent, const char* key) -> std::optional<Coord> {
    const auto c = d.find(parent, key);
    if (c == sexpr::kNoNode) return std::nullopt;
    return d.nm_at(c, 1);
  };
  if (const auto setup = d.find(d.root(), "setup"); setup != sexpr::kNoNode) {
    // KiCad 10.0.6 parseSetup sets m_LegacyDesignSettingsLoaded for exactly these tokens.
    // Local mask/paste settings, origins, stackup and plot settings do not suppress project rules.
    for (const char* key : {
             "user_trace_width", "trace_clearance", "zone_clearance", "zone_45_only", "clearance_min",
             "trace_min", "via_size", "via_drill", "via_min_annulus", "via_min_size", "through_hole_min",
             "via_min_drill", "hole_to_hole_min", "user_via", "uvia_size", "uvia_drill", "uvias_allowed",
             "blind_buried_vias_allowed", "uvia_min_size", "uvia_min_drill", "user_diff_pair", "segment_width",
             "edge_width", "mod_edge_width", "pcb_text_width", "mod_text_width", "pcb_text_size",
             "mod_text_size", "defaults", "pad_size", "pad_drill", "max_error"}) {
      if (d.find(setup, key) != sexpr::kNoNode) {
        legacy.design_settings = true;
        break;
      }
    }
    if (auto v = nm(setup, "clearance_min")) r.minimums.clearance = *v;
    if (auto v = nm(setup, "trace_min")) r.minimums.track_width = *v;
    if (auto v = nm(setup, "via_min_size")) r.minimums.via_diameter = *v;
    if (auto v = nm(setup, "via_min_annulus")) r.minimums.via_annular_width = *v;
    if (auto v = nm(setup, "via_min_drill")) r.minimums.through_hole_diameter = *v;
    if (auto v = nm(setup, "through_hole_min")) r.minimums.through_hole_diameter = *v;
    if (auto v = nm(setup, "hole_to_hole_min")) r.minimums.hole_to_hole = *v;
    if (auto v = nm(setup, "uvia_min_size")) r.minimums.microvia_diameter = *v;
    if (auto v = nm(setup, "uvia_min_drill")) r.minimums.microvia_drill = *v;
    if (auto v = nm(setup, "edge_clearance")) r.minimums.copper_edge_clearance = *v;
    if (const auto defaults = d.find(setup, "defaults"); defaults != sexpr::kNoNode)
      if (auto v = nm(defaults, "edge_clearance")) r.minimums.copper_edge_clearance = *v;
    if (auto v = nm(setup, "solder_mask_min_width")) r.minimums.solder_mask_min_width = *v;
    if (const auto bb = d.find(setup, "blind_buried_vias_allowed"); bb != sexpr::kNoNode) r.minimums.allow_blind_buried_vias = d.str_at(bb, 1) == "yes";
    if (const auto uv = d.find(setup, "uvias_allowed"); uv != sexpr::kNoNode) r.minimums.allow_microvias = d.str_at(uv, 1) == "yes";
  }
  for (const auto nc : d.find_all(d.root(), "net_class")) {
    legacy.netclasses = true;  // BOARD::SetProject migrates these over project classes and patterns.
    model::NetClass k;
    k.track_width = 200'000;  // KiCad 10 common/netclass.cpp DEFAULT_TRACK_WIDTH
    k.name = d.str_at(nc, 1);
    k.priority = k.name == "Default" ? INT_MAX : 0;
    if (auto v = nm(nc, "clearance")) k.clearance = *v;
    if (auto v = nm(nc, "trace_width")) k.track_width = *v;
    if (auto v = nm(nc, "via_dia")) k.via_diameter = *v;
    if (auto v = nm(nc, "via_drill")) k.via_drill = *v;
    if (auto v = nm(nc, "uvia_dia")) k.uvia_diameter = *v;
    if (auto v = nm(nc, "uvia_drill")) k.uvia_drill = *v;
    if (auto v = nm(nc, "diff_pair_width")) k.diff_pair_width = *v;
    if (auto v = nm(nc, "diff_pair_gap")) {
      k.diff_pair_gap = *v;
      k.has_diff_pair_gap = true;
    }
    for (const auto an : d.find_all(nc, "add_net")) r.assignments[d.str_at(an, 1)] = {k.name};
    if (k.name == "Default") r.classes.insert(r.classes.begin(), k);
    else r.classes.push_back(k);
  }
  return legacy;
}

}  // namespace

model::DesignRules read_design_rules(const std::string& board_path) {
  model::DesignRules r;
  const fs::path b(board_path);
  const fs::path pro = fs::path(b).replace_extension(".kicad_pro");
  const fs::path dru = fs::path(b).replace_extension(".kicad_dru");
  kicad_defaults(r);
  const auto legacy = read_legacy_board_rules(b, r);
  if (fs::exists(pro)) {
    read_pro(pro, r, legacy);
  } else {
    r.warnings.push_back("no project file " + pro.filename().string() + "; using KiCad defaults and rules stored in the board");
  }
  if (r.classes.empty() || r.classes.front().name != "Default") {
    model::NetClass def;
    def.track_width = 200'000;  // KiCad 10 common/netclass.cpp DEFAULT_TRACK_WIDTH
    def.name = "Default";
    def.priority = INT_MAX;
    r.classes.insert(r.classes.begin(), def);
  }
  if (fs::exists(dru)) read_dru(dru, r);
  return r;
}

}  // namespace tmk::io
