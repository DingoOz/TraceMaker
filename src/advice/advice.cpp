// SPDX-License-Identifier: GPL-3.0-or-later
// The advice file (doc 17): strict parsing, checking against the board, and the translation into router options.
#include "advice/advice.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace tmk::advice {

namespace {

using nlohmann::ordered_json;

[[noreturn]] void fail(const std::string& source, const std::string& where, const std::string& msg) {
  throw std::runtime_error(source + (where.empty() ? "" : ": " + where) + ": " + msg);
}

const char* json_type(const ordered_json& v) {
  if (v.is_number_integer()) return "integer";
  if (v.is_number()) return "number";
  if (v.is_boolean()) return "boolean";
  if (v.is_string()) return "string";
  if (v.is_array()) return "list";
  if (v.is_object()) return "map";
  return "null";
}

void check_keys(const std::string& source, const std::string& where, const ordered_json& obj, const std::set<std::string>& allowed) {
  for (const auto& [k, v] : obj.items()) {
    (void)v;
    if (allowed.count(k)) continue;
    std::string list;
    for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
    fail(source, where, "unknown key '" + k + "' (allowed: " + list + ")");
  }
}

// Text that ends up in messages, events and prompts: no control characters, bounded length.
bool plain_text(const std::string& s) {
  return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string need_name(const std::string& source, const std::string& where, const ordered_json& v, std::size_t max_len = 256) {
  if (!v.is_string()) fail(source, where, std::string("must be a string, got ") + json_type(v));
  const auto s = v.get<std::string>();
  if (s.empty()) fail(source, where, "must not be empty");
  if (s.size() > max_len) fail(source, where, "is longer than " + std::to_string(max_len) + " characters");
  if (!plain_text(s)) fail(source, where, "contains a control character");
  return s;
}

std::vector<std::string> name_list(const std::string& source, const std::string& where, const ordered_json& v, std::size_t max_count) {
  if (!v.is_array()) fail(source, where, std::string("must be a list of names, got ") + json_type(v));
  if (v.size() > max_count) fail(source, where, "has more than " + std::to_string(max_count) + " entries");
  std::vector<std::string> out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    const std::string w = where + "[" + std::to_string(i) + "]";
    std::string n = need_name(source, w, v[i]);
    if (std::find(out.begin(), out.end(), n) != out.end()) fail(source, w, "'" + n + "' is listed twice");
    out.push_back(std::move(n));
  }
  return out;
}

double need_number(const std::string& source, const std::string& where, const ordered_json& v, double lo, double hi) {
  if (!v.is_number()) fail(source, where, std::string("must be a number, got ") + json_type(v));
  const double d = v.get<double>();
  if (!std::isfinite(d) || d < lo || d > hi) {
    std::ostringstream m;
    m << "must be between " << lo << " and " << hi;
    fail(source, where, m.str());
  }
  return d;
}

bool need_bool(const std::string& source, const std::string& where, const ordered_json& v) {
  if (!v.is_boolean()) fail(source, where, std::string("must be true or false, got ") + json_type(v));
  return v.get<bool>();
}

// A board coordinate in millimetres: finite, within ten metres of the origin.
geom::Point need_point(const std::string& source, const std::string& where, const ordered_json& v) {
  if (!v.is_array() || v.size() != 2) fail(source, where, "must be [x, y] in millimetres");
  return {mm_to_nm(need_number(source, where + "[0]", v[0], -1e4, 1e4)), mm_to_nm(need_number(source, where + "[1]", v[1], -1e4, 1e4))};
}

double polygon_area2(const std::vector<geom::Point>& p) {  // twice the signed area, nm^2
  double a = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto& q = p[i];
    const auto& r = p[(i + 1) % p.size()];
    a += static_cast<double>(q.x) * static_cast<double>(r.y) - static_cast<double>(r.x) * static_cast<double>(q.y);
  }
  return a;
}

Region parse_region(const std::string& source, const std::string& where, const ordered_json& j) {
  if (!j.is_object()) fail(source, where, std::string("must be a map, got ") + json_type(j));
  check_keys(source, where, j, {"name", "note", "layers", "rect", "polygon", "reserve_for", "avoid_nets", "tracks", "vias"});
  Region r;
  r.where = where;
  if (j.contains("name")) r.name = need_name(source, where + ".name", j["name"], 80);
  if (j.contains("note")) (void)need_name(source, where + ".note", j["note"], kMaxNote);
  if (j.contains("layers")) {
    r.layers = name_list(source, where + ".layers", j["layers"], 64);
    if (r.layers.empty()) fail(source, where + ".layers", "must not be empty (leave it out for every copper layer)");
  }
  const bool has_rect = j.contains("rect"), has_poly = j.contains("polygon");
  if (has_rect == has_poly) fail(source, where, "needs exactly one of 'rect' and 'polygon'");
  if (has_rect) {
    const auto& v = j["rect"];
    if (!v.is_array() || v.size() != 4) fail(source, where + ".rect", "must be [left, top, right, bottom] in millimetres");
    const double x0 = need_number(source, where + ".rect[0]", v[0], -1e4, 1e4), y0 = need_number(source, where + ".rect[1]", v[1], -1e4, 1e4);
    const double x1 = need_number(source, where + ".rect[2]", v[2], -1e4, 1e4), y1 = need_number(source, where + ".rect[3]", v[3], -1e4, 1e4);
    const Coord ax = mm_to_nm(std::min(x0, x1)), bx = mm_to_nm(std::max(x0, x1)), ay = mm_to_nm(std::min(y0, y1)), by = mm_to_nm(std::max(y0, y1));
    if (ax == bx || ay == by) fail(source, where + ".rect", "has no area");
    r.polygon = {{ax, ay}, {bx, ay}, {bx, by}, {ax, by}};
  } else {
    const auto& v = j["polygon"];
    if (!v.is_array() || v.size() < 3 || v.size() > kMaxPolygon)
      fail(source, where + ".polygon", "must be a list of 3 to " + std::to_string(kMaxPolygon) + " points [x, y] in millimetres");
    for (std::size_t i = 0; i < v.size(); ++i) r.polygon.push_back(need_point(source, where + ".polygon[" + std::to_string(i) + "]", v[i]));
    if (std::abs(polygon_area2(r.polygon)) < 1e6) fail(source, where + ".polygon", "has no area");
  }
  const bool has_res = j.contains("reserve_for"), has_avoid = j.contains("avoid_nets");
  if (has_res == has_avoid) fail(source, where, "needs exactly one of 'reserve_for' (only these nets may enter) and 'avoid_nets' (these nets may not enter)");
  if (has_res) r.reserve_for = name_list(source, where + ".reserve_for", j["reserve_for"], kMaxRegionNets);
  if (has_avoid) r.avoid_nets = name_list(source, where + ".avoid_nets", j["avoid_nets"], kMaxRegionNets);
  if ((has_res ? r.reserve_for : r.avoid_nets).empty()) fail(source, where + (has_res ? ".reserve_for" : ".avoid_nets"), "must name at least one net");
  if (j.contains("tracks")) r.tracks = need_bool(source, where + ".tracks", j["tracks"]);
  if (j.contains("vias")) r.vias = need_bool(source, where + ".vias", j["vias"]);
  if (!r.tracks && !r.vias) fail(source, where, "keeps out neither tracks nor vias");
  return r;
}

// Levenshtein distance for "did you mean" hints.
std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diag = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t up = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diag = up;
    }
  }
  return row[b.size()];
}

std::string closest(std::string_view got, const std::vector<std::string>& candidates) {
  std::string best;
  std::size_t bd = 4;
  for (const auto& c : candidates) {
    const std::size_t d = edit_distance(got, c);
    if (d < bd) bd = d, best = c;
  }
  return best.empty() ? std::string() : " (did you mean " + best + "?)";
}

std::string pad_label(const model::Board& b, const model::Pad& p) {
  return p.footprint >= 0 ? b.footprints[static_cast<std::size_t>(p.footprint)].reference + "." + p.number : p.number;
}

}  // namespace

Advice parse_advice(std::string_view text, const std::string& source) {
  ordered_json j;
  try {
    j = ordered_json::parse(text);
  } catch (const nlohmann::json::parse_error& e) {
    // Byte offset to line and column, so the message points into the file.
    const std::size_t at = std::min<std::size_t>(e.byte > 0 ? e.byte - 1 : 0, text.size());
    const auto line = 1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n');
    const auto col = at - (text.substr(0, at).rfind('\n') == std::string_view::npos ? 0 : text.substr(0, at).rfind('\n') + 1) + 1;
    fail(source, "line " + std::to_string(line) + ", column " + std::to_string(col), std::string("not valid JSON: ") + e.what());
  }
  if (!j.is_object()) fail(source, "", std::string("must be a JSON map, got ") + json_type(j));
  check_keys(source, "", j, {"version", "note", "variants", "first_nets", "priority", "layers", "regions"});
  Advice a;
  a.source = source;
  if (!j.contains("version")) fail(source, "version", "is required (this engine reads version 1)");
  if (!j["version"].is_number_integer() || j["version"].get<int>() != 1) fail(source, "version", "must be 1");
  if (j.contains("note")) a.note = need_name(source, "note", j["note"], kMaxNote);
  if (j.contains("variants")) {
    const auto& v = j["variants"];
    if (!v.is_number_integer() || v.get<int>() < 1 || v.get<int>() > kMaxVariants)
      fail(source, "variants", "must be an integer from 1 to " + std::to_string(kMaxVariants));
    a.variants = v.get<int>();
  }
  if (j.contains("first_nets")) a.first_nets = name_list(source, "first_nets", j["first_nets"], kMaxFirstNets);
  if (j.contains("priority")) {
    const auto& v = j["priority"];
    if (!v.is_array()) fail(source, "priority", std::string("must be a list of pad pairs, got ") + json_type(v));
    if (v.size() > kMaxPriority) fail(source, "priority", "has more than " + std::to_string(kMaxPriority) + " entries");
    for (std::size_t i = 0; i < v.size(); ++i) {
      const std::string w = "priority[" + std::to_string(i) + "]";
      std::pair<std::string, std::string> pr;
      if (v[i].is_array()) {
        if (v[i].size() != 2) fail(source, w, "must be [\"REF.PIN\", \"REF.PIN\"]");
        pr = {need_name(source, w + "[0]", v[i][0]), need_name(source, w + "[1]", v[i][1])};
      } else if (v[i].is_object()) {
        check_keys(source, w, v[i], {"a", "b"});
        if (!v[i].contains("a") || !v[i].contains("b")) fail(source, w, "needs 'a' and 'b'");
        pr = {need_name(source, w + ".a", v[i]["a"]), need_name(source, w + ".b", v[i]["b"])};
      } else {
        fail(source, w, std::string("must be a pair of pads, got ") + json_type(v[i]));
      }
      if (pr.first == pr.second) fail(source, w, "names the same pad twice");
      for (const auto& other : a.priority)
        if ((other.first == pr.first && other.second == pr.second) || (other.first == pr.second && other.second == pr.first))
          fail(source, w, "'" + pr.first + "' and '" + pr.second + "' are listed twice");
      a.priority.push_back(std::move(pr));
    }
  }
  if (j.contains("layers")) {
    const auto& v = j["layers"];
    if (!v.is_object()) fail(source, "layers", std::string("must be a map, got ") + json_type(v));
    check_keys(source, "layers", v, {"no_tracks", "cost"});
    if (v.contains("no_tracks")) a.no_tracks = name_list(source, "layers.no_tracks", v["no_tracks"], 64);
    if (v.contains("cost")) {
      const auto& c = v["cost"];
      if (!c.is_object()) fail(source, "layers.cost", std::string("must be a map of layer to factor, got ") + json_type(c));
      for (const auto& [name, f] : c.items()) {
        if (name.empty() || name.size() > 64 || !plain_text(name)) fail(source, "layers.cost", "has a bad layer name");
        a.layer_cost.emplace_back(name, need_number(source, "layers.cost." + name, f, 1.0, 1000.0));
      }
    }
  }
  if (j.contains("regions")) {
    const auto& v = j["regions"];
    if (!v.is_array()) fail(source, "regions", std::string("must be a list, got ") + json_type(v));
    if (v.size() > kMaxRegions) fail(source, "regions", "has more than " + std::to_string(kMaxRegions) + " entries");
    for (std::size_t i = 0; i < v.size(); ++i) a.regions.push_back(parse_region(source, "regions[" + std::to_string(i) + "]", v[i]));
  }
  return a;
}

Advice load_advice_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error(path + ": cannot read the advice file");
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (text.size() > (1u << 20)) throw std::runtime_error(path + ": advice file is larger than 1 MB");
  return parse_advice(text, path);
}

void check_advice_refs(const Advice& a, const model::Board& b, std::vector<std::string>* warnings) {
  std::vector<std::string> problems;
  std::vector<std::string> net_names, layer_names;
  for (const auto& n : b.nets)
    if (n.id > 0 && !n.name.empty()) net_names.push_back(n.name);
  for (int l = 0; l < b.copper_count(); ++l) layer_names.push_back(b.copper_name(l));
  auto check_net = [&](const std::string& where, const std::string& name) {
    if (b.net_by_name(name) == 0) problems.push_back(where + ": no net named '" + name + "'" + closest(name, net_names));
  };
  auto check_layer = [&](const std::string& where, const std::string& name) {
    if (b.copper_index(name) < 0) {
      std::string list;
      for (const auto& n : layer_names) list += (list.empty() ? "" : ", ") + n;
      problems.push_back(where + ": no copper layer named '" + name + "'" + closest(name, layer_names) + " (the board has: " + list + ")");
    }
  };
  for (std::size_t i = 0; i < a.first_nets.size(); ++i) check_net("first_nets[" + std::to_string(i) + "]", a.first_nets[i]);
  // Pads by their label, "REF.NUM" as the router writes it.
  std::unordered_map<std::string, int> pads;
  std::vector<std::string> labels;
  if (!a.priority.empty())
    for (std::size_t i = 0; i < b.pads.size(); ++i) {
      const auto& p = b.pads[i];
      if (p.footprint < 0 || p.net == 0) continue;
      pads.emplace(pad_label(b, p), static_cast<int>(i));
      labels.push_back(pad_label(b, p));
    }
  for (std::size_t i = 0; i < a.priority.size(); ++i) {
    const std::string w = "priority[" + std::to_string(i) + "]";
    int idx[2] = {-1, -1};
    const std::string* names[2] = {&a.priority[i].first, &a.priority[i].second};
    for (int k = 0; k < 2; ++k) {
      const auto it = pads.find(*names[k]);
      if (it == pads.end()) problems.push_back(w + ": no pad '" + *names[k] + "' on a net" + closest(*names[k], labels));
      else idx[k] = it->second;
    }
    if (idx[0] >= 0 && idx[1] >= 0 && b.pads[static_cast<std::size_t>(idx[0])].net != b.pads[static_cast<std::size_t>(idx[1])].net)
      problems.push_back(w + ": '" + *names[0] + "' and '" + *names[1] + "' are on different nets, so no connection joins them");
  }
  for (std::size_t i = 0; i < a.no_tracks.size(); ++i) check_layer("layers.no_tracks[" + std::to_string(i) + "]", a.no_tracks[i]);
  for (const auto& [name, f] : a.layer_cost) {
    (void)f;
    check_layer("layers.cost." + name, name);
  }
  for (const auto& r : a.regions) {
    for (std::size_t i = 0; i < r.layers.size(); ++i) check_layer(r.where + ".layers[" + std::to_string(i) + "]", r.layers[i]);
    for (std::size_t i = 0; i < r.reserve_for.size(); ++i) check_net(r.where + ".reserve_for[" + std::to_string(i) + "]", r.reserve_for[i]);
    for (std::size_t i = 0; i < r.avoid_nets.size(); ++i) check_net(r.where + ".avoid_nets[" + std::to_string(i) + "]", r.avoid_nets[i]);
    // A net name the rule language cannot quote would end the condition early.
    for (const auto* list : {&r.reserve_for, &r.avoid_nets})
      for (const auto& n : *list)
        if (n.find_first_of("'\"\\") != std::string::npos) problems.push_back(r.where + ": net '" + n + "' has a quote or backslash in its name and cannot be used in a region");
  }
  if (!a.no_tracks.empty()) {
    std::set<int> off;
    for (const auto& n : a.no_tracks)
      if (const int l = b.copper_index(n); l >= 0) off.insert(l);
    if (static_cast<int>(off.size()) >= b.copper_count()) problems.push_back("layers.no_tracks: every copper layer would get no tracks");
  }
  if (!problems.empty()) {
    std::string msg = a.source + ": " + std::to_string(problems.size()) + " problem" + (problems.size() == 1 ? "" : "s") + " against the board:";
    for (std::size_t i = 0; i < problems.size() && i < 12; ++i) msg += "\n  " + problems[i];
    if (problems.size() > 12) msg += "\n  ... and " + std::to_string(problems.size() - 12) + " more";
    throw std::runtime_error(msg);
  }
  if (!warnings) return;
  const auto bbox = b.edge_bbox();
  for (const auto& r : a.regions) {
    geom::Box box;
    for (const auto& p : r.polygon) box.add(p);
    if (!bbox.empty() && (box.x1 < bbox.x0 || box.x0 > bbox.x1 || box.y1 < bbox.y0 || box.y0 > bbox.y1))
      warnings->push_back(r.where + ": lies outside the board outline and has no effect");
  }
  std::set<std::string> seen;
  for (const auto& n : a.first_nets) {
    if (!seen.insert(n).second) continue;
    int pads_on_net = 0;
    for (const auto& p : b.pads) pads_on_net += p.net == b.net_by_name(n);
    if (pads_on_net < 2) warnings->push_back("first_nets: '" + n + "' has fewer than two pads, so there is nothing to route first");
  }
}

Applied apply_advice(const Advice& a, const model::Board& b) {
  Applied out;
  check_advice_refs(a, b, &out.warnings);
  auto& s = out.set;
  s.source = a.source;
  s.variants = a.variants;
  s.priority = a.priority;
  s.first_nets = a.first_nets;
  const int nl = b.copper_count();
  for (const auto& n : a.no_tracks) s.no_track_layers |= model::layer_bit(b.copper_index(n));
  if (!a.layer_cost.empty()) {
    s.layer_cost_pm.assign(static_cast<std::size_t>(nl), 1000);
    for (const auto& [n, f] : a.layer_cost) {
      auto& pm = s.layer_cost_pm[static_cast<std::size_t>(b.copper_index(n))];
      pm = std::max(pm, static_cast<std::int32_t>(std::llround(f * 1000.0)));
    }
  }
  int number = 0;
  for (const auto& r : a.regions) {
    ++number;
    model::Zone z;
    z.rule_area = true;  // no keep-out flags: the area only names a place for the rules below
    z.name = "tmk:advice:" + std::to_string(number);
    model::LayerMask mask = 0;
    std::vector<int> layers;
    if (r.layers.empty()) {
      for (int l = 0; l < nl; ++l) layers.push_back(l);
    } else {
      for (const auto& n : r.layers) layers.push_back(b.copper_index(n));
    }
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
    for (int l : layers) {
      mask |= model::layer_bit(l);
      z.layers.push_back(b.copper_name(l));
    }
    z.copper = mask;
    z.outline.push_back(r.polygon);
    s.zones.push_back(std::move(z));
    // One rule per layer when the region names layers (a rule has one layer field), one without a layer for all of them.
    std::vector<std::string> rule_layers;
    if (r.layers.empty() || static_cast<int>(layers.size()) == nl) rule_layers = {""};
    else
      for (int l : layers) rule_layers.push_back(b.copper_name(l));
    model::Constraint disallow;
    disallow.type = "disallow";
    if (r.tracks) disallow.items.push_back("track");
    if (r.vias) disallow.items.push_back("via");
    const std::string area = "A.intersectsArea('tmk:advice:" + std::to_string(number) + "')";
    auto add_rule = [&](const std::string& tag, const std::string& condition) {
      for (const auto& layer : rule_layers) {
        model::CustomRule cr;
        cr.name = "tmk:advice:" + std::to_string(number) + ":" + tag;
        cr.condition = condition;
        cr.layer = layer;
        cr.constraints.push_back(disallow);
        s.rules.push_back(std::move(cr));
      }
    };
    if (!r.reserve_for.empty()) {
      // `a && b && c`: the engine's grammar has no parentheses for a list of nets, so "every net but these" is a conjunction of
      // inequalities (and KiCad reads `a && b || c` as `a && (b || c)`, so no `||` is used anywhere).
      std::string cond = area;
      for (const auto& n : r.reserve_for) cond += " && A.NetName != '" + n + "'";
      add_rule("reserve", cond);
    }
    for (const auto& n : r.avoid_nets) add_rule("avoid", area + " && A.NetName == '" + n + "'");
  }
  // The report: one line per hint, with what the router will do with it.
  out.report = {{"source", a.source}, {"note", a.note}, {"variants", a.variants}, {"hints", ordered_json::array()}};
  auto hint = [&](const char* kind, std::string text) { out.report["hints"].push_back({{"kind", kind}, {"text", std::move(text)}}); };
  if (!a.first_nets.empty()) {
    std::string t;
    for (const auto& n : a.first_nets) t += (t.empty() ? "" : ", ") + n;
    hint("first_nets", "route first: " + t);
  }
  for (const auto& p : a.priority) hint("priority", "route first: " + p.first + " - " + p.second);
  if (!a.no_tracks.empty()) {
    std::string t;
    for (const auto& n : a.no_tracks) t += (t.empty() ? "" : ", ") + n;
    hint("no_tracks", "no new tracks on: " + t);
  }
  for (const auto& [n, f] : a.layer_cost) {
    std::ostringstream t;
    t << "track cost on " << n << ": x" << f;
    hint("layer_cost", t.str());
  }
  for (const auto& r : a.regions) {
    const auto& nets = !r.reserve_for.empty() ? r.reserve_for : r.avoid_nets;
    std::string t = (r.name.empty() ? r.where : r.name) + ": " + (!r.reserve_for.empty() ? "reserved for " : "kept clear of ");
    for (std::size_t i = 0; i < nets.size(); ++i) t += (i ? ", " : "") + nets[i];
    hint("region", t);
  }
  return out;
}

nlohmann::json advice_to_json(const Advice& a) {
  ordered_json j;
  j["version"] = 1;
  if (!a.note.empty()) j["note"] = a.note;
  j["variants"] = a.variants;
  if (!a.first_nets.empty()) j["first_nets"] = a.first_nets;
  if (!a.priority.empty()) {
    j["priority"] = ordered_json::array();
    for (const auto& [x, y] : a.priority) j["priority"].push_back(ordered_json::array({x, y}));
  }
  if (!a.no_tracks.empty() || !a.layer_cost.empty()) {
    ordered_json l = ordered_json::object();
    if (!a.no_tracks.empty()) l["no_tracks"] = a.no_tracks;
    if (!a.layer_cost.empty()) {
      l["cost"] = ordered_json::object();
      for (const auto& [n, f] : a.layer_cost) l["cost"][n] = f;
    }
    j["layers"] = l;
  }
  if (!a.regions.empty()) {
    j["regions"] = ordered_json::array();
    for (const auto& r : a.regions) {
      ordered_json rj;
      if (!r.name.empty()) rj["name"] = r.name;
      if (!r.layers.empty()) rj["layers"] = r.layers;
      ordered_json poly = ordered_json::array();
      for (const auto& p : r.polygon) poly.push_back(ordered_json::array({nm_to_mm(p.x), nm_to_mm(p.y)}));
      rj["polygon"] = poly;
      if (!r.reserve_for.empty()) rj["reserve_for"] = r.reserve_for;
      if (!r.avoid_nets.empty()) rj["avoid_nets"] = r.avoid_nets;
      rj["tracks"] = r.tracks;
      rj["vias"] = r.vias;
      j["regions"].push_back(std::move(rj));
    }
  }
  return j;
}

nlohmann::json advice_schema() {
  return ordered_json::parse(R"JSON({
  "format": "JSON map with exactly these keys; unknown keys, wrong types and unknown names are refused. Millimetres, the board file's axes (y grows downwards).",
  "version": {"type": "integer", "value": 1, "required": true},
  "note": {"type": "string", "about": "free text, logged and never acted on"},
  "variants": {"type": "integer", "range": [1, 8], "default": 4, "about": "advised router variants run next to the unadvised ones"},
  "first_nets": {"type": "list of net names", "max": 64, "about": "routed first, and other nets do not rip them; use for a few critical nets"},
  "priority": {"type": "list of [\"REF.PIN\", \"REF.PIN\"] pairs on one net", "max": 512, "about": "these connections are routed before the rest (listed in the report as unrouted: from and to)"},
  "layers": {"no_tracks": {"type": "list of copper layer names", "about": "no new tracks there; vias still pass"},
             "cost": {"type": "map layer name to factor", "range": [1, 1000], "about": "a track on that layer costs this many times more"}},
  "regions": {"type": "list", "max": 24,
              "each": {"name": "string, optional",
                       "rect": "[left, top, right, bottom] in mm, or polygon: [[x, y], ...] with 3 to 64 points; exactly one of the two",
                       "layers": "copper layer names; default all",
                       "reserve_for": "net names that alone may put copper there (all other nets are kept out); exactly one of reserve_for and avoid_nets",
                       "avoid_nets": "net names kept out of the region",
                       "tracks": "true by default", "vias": "true by default"}},
  "never": "copper, rule values, commands, file names. Advice only changes the order of connections, the cost of a layer, and where some nets may not go."
})JSON");
}

void unalias(Advice& a, const AliasMap& m) {
  auto net = [&](std::string& n) {
    if (const auto it = m.net.find(n); it != m.net.end()) n = it->second;
  };
  auto pad = [&](std::string& p) {
    const auto dot = p.find('.');
    if (dot == std::string::npos) return;
    if (const auto it = m.part.find(p.substr(0, dot)); it != m.part.end()) p = it->second + p.substr(dot);
  };
  for (auto& n : a.first_nets) net(n);
  for (auto& [x, y] : a.priority) pad(x), pad(y);
  for (auto& r : a.regions) {
    for (auto& n : r.reserve_for) net(n);
    for (auto& n : r.avoid_nets) net(n);
  }
}

}  // namespace tmk::advice
