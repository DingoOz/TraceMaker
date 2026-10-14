// SPDX-License-Identifier: GPL-3.0-or-later
// Datasheet land-pattern references and trusted brands (issue #5, doc 05 §43): see footprint_ref.hpp.
#include "analysis/footprint_ref.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "io/kicad/board_reader.hpp"

namespace tmk::analysis {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

// A verification tool must not ignore a misspelt key: a tolerance that is silently the default is a wrong answer.
void only_keys(const json& o, std::initializer_list<const char*> allowed, const std::string& where) {
  if (!o.is_object()) fail(where, "expected an object");
  for (auto it = o.begin(); it != o.end(); ++it) {
    bool ok = false;
    for (const char* k : allowed) ok = ok || it.key() == k;
    if (!ok) fail(where, "unknown key '" + it.key() + "'");
  }
}

std::string str_of(const json& o, const char* key, const std::string& where, bool required) {
  const auto it = o.find(key);
  if (it == o.end() || it->is_null()) {
    if (required) fail(where, std::string("missing '") + key + "'");
    return "";
  }
  if (!it->is_string()) fail(where, std::string("'") + key + "' must be a string");
  return it->get<std::string>();
}

double num_of(const json& v, const std::string& where, double lo, double hi) {
  if (!v.is_number()) fail(where, "expected a number");
  const double d = v.get<double>();
  if (!std::isfinite(d) || d < lo || d > hi) fail(where, "number out of range: " + v.dump());
  return d;
}

std::vector<std::string> patterns_of(const json& o, const char* key, const std::string& where) {
  std::vector<std::string> out;
  const auto it = o.find(key);
  if (it == o.end() || it->is_null()) return out;
  if (it->is_string()) {
    out.push_back(it->get<std::string>());
  } else if (it->is_array()) {
    for (const auto& e : *it) {
      if (!e.is_string()) fail(where, std::string("'") + key + "' must hold strings");
      out.push_back(e.get<std::string>());
    }
  } else {
    fail(where, std::string("'") + key + "' must be a string or a list of strings");
  }
  for (const auto& p : out)
    if (p.empty()) fail(where, std::string("'") + key + "' has an empty pattern (it would match everything)");
  return out;
}

constexpr double kNmPerMm = 1e6;

RefPad pad_of(const json& o, const std::string& where) {
  only_keys(o, {"number", "type", "shape", "at", "size", "angle", "drill"}, where);
  RefPad p;
  if (!o.contains("number") || !o["number"].is_string()) fail(where, "'number' must be a string (\"\" for an unnumbered hole)");
  p.number = o["number"].get<std::string>();
  const std::string type = str_of(o, "type", where, false);
  if (type.empty() || type == "smd") p.type = model::PadType::Smd;
  else if (type == "thru_hole") p.type = model::PadType::ThruHole;
  else if (type == "np_thru_hole") p.type = model::PadType::NpThruHole;
  else fail(where, "'type' is smd, thru_hole or np_thru_hole, not '" + type + "'");
  const std::string shape = str_of(o, "shape", where, false);
  if (shape.empty() || shape == "rect") p.shape = model::PadShape::Rect;
  else if (shape == "circle") p.shape = model::PadShape::Circle;
  else if (shape == "oval") p.shape = model::PadShape::Oval;
  else if (shape == "roundrect") p.shape = model::PadShape::RoundRect;
  else if (shape == "chamfered_rect") p.shape = model::PadShape::ChamferedRect;
  else if (shape == "trapezoid") p.shape = model::PadShape::Trapezoid;
  else if (shape == "custom") p.shape = model::PadShape::Custom;
  else fail(where, "unknown 'shape' '" + shape + "'");
  if (!o.contains("at") || !o["at"].is_array() || o["at"].size() != 2) fail(where, "'at' must be [x, y] in millimetres");
  p.x = num_of(o["at"][0], where + " at.x", -2000, 2000) * kNmPerMm;
  p.y = num_of(o["at"][1], where + " at.y", -2000, 2000) * kNmPerMm;
  if (!o.contains("size")) fail(where, "missing 'size'");
  if (o["size"].is_number()) {
    p.w = p.h = num_of(o["size"], where + " size", 0.001, 1000) * kNmPerMm;
  } else if (o["size"].is_array() && o["size"].size() == 2) {
    p.w = num_of(o["size"][0], where + " size.w", 0.001, 1000) * kNmPerMm;
    p.h = num_of(o["size"][1], where + " size.h", 0.001, 1000) * kNmPerMm;
  } else {
    fail(where, "'size' must be a number or [w, h] in millimetres");
  }
  if (o.contains("angle")) p.angle = num_of(o["angle"], where + " angle", -3600, 3600);
  if (o.contains("drill") && !o["drill"].is_null()) {
    if (o["drill"].is_number()) {
      p.drill_w = p.drill_h = num_of(o["drill"], where + " drill", 0.001, 1000) * kNmPerMm;
    } else if (o["drill"].is_array() && o["drill"].size() == 2) {
      p.drill_w = num_of(o["drill"][0], where + " drill.w", 0.001, 1000) * kNmPerMm;
      p.drill_h = num_of(o["drill"][1], where + " drill.h", 0.001, 1000) * kNmPerMm;
    } else {
      fail(where, "'drill' must be a number or [w, h] in millimetres");
    }
  }
  if (p.type != model::PadType::Smd && p.drill_w <= 0) fail(where, "a through hole needs a 'drill'");
  if (p.type == model::PadType::Smd && p.drill_w > 0) fail(where, "an smd pad has no 'drill'");
  return p;
}

FootprintRef entry_of(const json& o, const fs::path& file, std::size_t index) {
  const std::string where = file.filename().string() + " references[" + std::to_string(index) + "]";
  only_keys(o, {"brand", "part", "package", "datasheet", "match", "pads", "footprint", "tolerance_mm"}, where);
  FootprintRef r;
  r.brand = str_of(o, "brand", where, true);
  r.part = str_of(o, "part", where, true);
  if (r.brand.empty() || r.part.empty()) fail(where, "'brand' and 'part' must not be empty");
  r.package = str_of(o, "package", where, false);
  r.source = file.string();
  r.id = file.filename().string() + "#" + r.part;
  if (!o.contains("datasheet")) fail(where, "missing 'datasheet': a reference that cannot be traced to a document is not a reference");
  const json& ds = o["datasheet"];
  only_keys(ds, {"document", "revision", "page", "figure", "url"}, where + " datasheet");
  r.document = str_of(ds, "document", where + " datasheet", true);
  if (r.document.empty()) fail(where, "'datasheet.document' must name the document");
  r.revision = str_of(ds, "revision", where + " datasheet", false);
  r.figure = str_of(ds, "figure", where + " datasheet", false);
  r.url = str_of(ds, "url", where + " datasheet", false);
  if (ds.contains("page")) r.page = static_cast<int>(num_of(ds["page"], where + " datasheet.page", 0, 100000));
  if (!o.contains("match")) fail(where, "missing 'match': which footprints is this the land pattern of?");
  const json& m = o["match"];
  only_keys(m, {"mpn", "lib_id", "value", "ref"}, where + " match");
  r.mpn = patterns_of(m, "mpn", where + " match");
  r.lib_id = patterns_of(m, "lib_id", where + " match");
  r.value = patterns_of(m, "value", where + " match");
  r.ref = patterns_of(m, "ref", where + " match");
  if (r.specificity() == 0) fail(where, "'match' selects nothing: give mpn, lib_id, value or ref");
  const bool has_pads = o.contains("pads"), has_fp = o.contains("footprint");
  if (has_pads == has_fp) fail(where, "give either 'pads' or 'footprint', not both and not neither");
  if (has_pads) {
    if (!o["pads"].is_array() || o["pads"].empty()) fail(where, "'pads' must be a non-empty list");
    std::size_t i = 0;
    for (const auto& p : o["pads"]) r.pads.push_back(pad_of(p, where + " pads[" + std::to_string(i++) + "]"));
  } else {
    if (!o["footprint"].is_string()) fail(where, "'footprint' must be a path");
    const fs::path fp = file.parent_path() / o["footprint"].get<std::string>();
    try {
      r.pads = pads_from_kicad_mod(read_text(fp));
    } catch (const std::exception& e) {
      fail(where, "footprint '" + fp.string() + "': " + e.what());
    }
    if (r.pads.empty()) fail(where, "footprint '" + fp.string() + "' has no pads");
  }
  if (o.contains("tolerance_mm")) {
    const json& t = o["tolerance_mm"];
    only_keys(t, {"position", "size", "drill"}, where + " tolerance_mm");
    if (t.contains("position")) r.position_tol_nm = num_of(t["position"], where + " tolerance_mm.position", 0, 10) * kNmPerMm;
    if (t.contains("size")) r.size_tol_nm = num_of(t["size"], where + " tolerance_mm.size", 0, 10) * kNmPerMm;
    if (t.contains("drill")) r.drill_tol_nm = num_of(t["drill"], where + " tolerance_mm.drill", 0, 10) * kNmPerMm;
  }
  return r;
}

}  // namespace

std::string normalise_brand(const std::string& brand) {
  static const std::set<std::string> suffix{"inc", "incorporated", "ltd", "limited", "llc", "gmbh", "corp", "corporation", "co", "company", "ag", "plc", "nv", "sa", "bv", "kk"};
  std::vector<std::string> words;
  std::string cur;
  for (const char c : brand) {
    if (c == '.') continue;  // "N.V." and "Inc." lose their dots: "nv", "inc"
    if (std::isalnum(static_cast<unsigned char>(c))) {
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else if (!cur.empty()) {
      words.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) words.push_back(cur);
  while (words.size() > 1 && suffix.count(words.back())) words.pop_back();
  if (words.size() > 1 && words.front() == "the") words.erase(words.begin());
  std::string out;
  for (const auto& w : words) out += (out.empty() ? "" : " ") + w;
  return out;
}

bool TrustedBrands::trusts(const std::string& brand) const {
  if (trust_all) return true;
  const std::string n = normalise_brand(brand);
  if (n.empty()) return false;
  for (const auto& g : groups)
    if (std::find(g.begin(), g.end(), n) != g.end()) return true;
  return false;
}

TrustedBrands parse_trusted_brands(const std::string& text, const std::string& source) {
  TrustedBrands t;
  t.source = source;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::vector<std::string> group;
    std::istringstream ls(line);
    std::string part;
    while (std::getline(ls, part, '|')) {
      const std::string n = normalise_brand(trim(part));
      if (!n.empty() && std::find(group.begin(), group.end(), n) == group.end()) group.push_back(n);
    }
    if (!group.empty()) t.groups.push_back(std::move(group));
  }
  return t;
}

TrustedBrands load_trusted_brands(const std::string& file) { return parse_trusted_brands(read_text(file), file); }

std::vector<RefPad> pads_from_kicad_mod(const std::string& text) {
  // The footprint file is read by the board reader as a footprint at the origin of an otherwise empty two-layer board: its pad coordinates are
  // then the footprint's own.
  const std::string wrapped = "(kicad_pcb (version 20240108) (generator \"tracemaker\") (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))\n" + text + "\n)";
  const auto doc = sexpr::Document::parse(wrapped);
  const model::Board b = io::read_board(doc);
  if (b.footprints.size() != 1) throw std::runtime_error("expected one footprint, found " + std::to_string(b.footprints.size()));
  std::vector<RefPad> out;
  for (const int pi : b.footprints[0].pads) {
    const model::Pad& p = b.pads[static_cast<std::size_t>(pi)];
    if (p.type != model::PadType::NpThruHole && p.copper == 0) continue;  // paste and mask apertures are not land pattern
    RefPad r;
    r.number = p.number;
    r.type = p.type == model::PadType::Connect ? model::PadType::Smd : p.type;
    r.shape = p.shape;
    r.x = static_cast<double>(p.pos.x);
    r.y = static_cast<double>(p.pos.y);
    r.angle = p.angle;
    r.w = static_cast<double>(p.size_x);
    r.h = static_cast<double>(p.size_y);
    r.drill_w = static_cast<double>(p.drill_x);
    r.drill_h = static_cast<double>(p.drill_y);
    out.push_back(std::move(r));
  }
  return out;
}

RefLibrary load_references(const std::string& path, const std::string& trusted_brands_file) {
  RefLibrary lib;
  std::vector<fs::path> files;
  fs::path trusted_beside;
  if (fs::is_directory(path)) {
    for (const auto& e : fs::recursive_directory_iterator(path))
      if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    trusted_beside = fs::path(path) / "trusted_brands.txt";
  } else if (fs::is_regular_file(path)) {
    files.push_back(path);
    trusted_beside = fs::path(path).parent_path() / "trusted_brands.txt";
  } else {
    throw std::runtime_error("no such reference file or directory: " + path);
  }
  for (const auto& f : files) {
    json j;
    try {
      j = json::parse(read_text(f));
    } catch (const json::parse_error& e) {
      fail(f.filename().string(), std::string("not valid JSON: ") + e.what());
    }
    only_keys(j, {"format", "references", "description"}, f.filename().string());
    if (str_of(j, "format", f.filename().string(), true) != "tracemaker-footprint-refs/1") fail(f.filename().string(), "'format' must be \"tracemaker-footprint-refs/1\"");
    if (!j.contains("references") || !j["references"].is_array()) fail(f.filename().string(), "'references' must be a list");
    std::size_t i = 0;
    for (const auto& e : j["references"]) lib.refs.push_back(entry_of(e, f, i++));
    lib.files.push_back(f.string());
  }
  if (!trusted_brands_file.empty()) lib.trusted = load_trusted_brands(trusted_brands_file);
  else if (fs::is_regular_file(trusted_beside)) lib.trusted = load_trusted_brands(trusted_beside.string());
  return lib;
}

}  // namespace tmk::analysis
