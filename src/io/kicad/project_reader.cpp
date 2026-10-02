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

void read_pro(const fs::path& p, model::DesignRules& r) {
  std::ifstream f(p);
  json d;
  try {
    d = json::parse(f);
  } catch (const json::exception& e) {
    r.warnings.push_back("cannot parse " + p.string() + ": " + e.what());
    return;
  }
  if (d.contains("board") && d["board"].contains("design_settings") && d["board"]["design_settings"].contains("rules")) {
    const json& j = d["board"]["design_settings"]["rules"];
    auto& m = r.minimums;
    m.clearance = mm(j, "min_clearance", 0);
    m.track_width = mm(j, "min_track_width", 0);
    m.connection_width = mm(j, "min_connection", 0);
    m.via_diameter = mm(j, "min_via_diameter", 0);
    m.via_annular_width = mm(j, "min_via_annular_width", 0);
    m.through_hole_diameter = mm(j, "min_through_hole_diameter", 0);
    m.microvia_diameter = mm(j, "min_microvia_diameter", 0);
    m.microvia_drill = mm(j, "min_microvia_drill", 0);
    m.hole_clearance = mm(j, "min_hole_clearance", 0);
    m.hole_to_hole = mm(j, "min_hole_to_hole", 0);
    m.copper_edge_clearance = mm(j, "min_copper_edge_clearance", 0);
    m.silk_clearance = mm(j, "min_silk_clearance", 0);
    m.solder_mask_to_copper_clearance = mm(j, "solder_mask_to_copper_clearance", 0);
    m.allow_blind_buried_vias = j.value("allow_blind_buried_vias", false);
    m.allow_microvias = j.value("allow_microvias", false);
  } else {
    r.warnings.push_back("project has no board design rules; using zeros");
  }
  if (!d.contains("net_settings")) return;
  const json& ns = d["net_settings"];
  if (ns.contains("classes") && ns["classes"].is_array()) {
    // Default first; other classes inherit unset (null) values from Default.
    model::NetClass def;
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
      }
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

}  // namespace

model::DesignRules read_design_rules(const std::string& board_path) {
  model::DesignRules r;
  const fs::path b(board_path);
  const fs::path pro = fs::path(b).replace_extension(".kicad_pro");
  const fs::path dru = fs::path(b).replace_extension(".kicad_dru");
  if (fs::exists(pro)) read_pro(pro, r);
  else r.warnings.push_back("no project file " + pro.filename().string() + "; using KiCad default net class");
  if (r.classes.empty()) {
    model::NetClass def;
    def.name = "Default";
    def.priority = INT_MAX;
    r.classes.push_back(def);
  }
  if (fs::exists(dru)) read_dru(dru, r);
  return r;
}

}  // namespace tmk::io
