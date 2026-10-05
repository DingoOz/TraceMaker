// SPDX-License-Identifier: GPL-3.0-or-later
// User override file (doc 15 §3.5, §6.3): parsing and validation. Applying the entries is part of detection
// (detect.cpp: assert/deny change which instances exist; disable/set are attached to the instances).
#include "crules/overrides.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace tmk::crules {

namespace {

using nlohmann::ordered_json;

// Levenshtein distance, for "did you mean" hints on typos.
std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diag = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t up = row[j];
      const bool same = std::tolower(static_cast<unsigned char>(a[i - 1])) == std::tolower(static_cast<unsigned char>(b[j - 1]));
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (same ? 0 : 1)});
      diag = up;
    }
  }
  return row[b.size()];
}

// " (did you mean X?)" for the closest candidate within distance 3, else "".
std::string hint(std::string_view got, const std::vector<std::string>& candidates) {
  std::string best;
  std::size_t bd = 4;
  for (const auto& c : candidates) {  // candidates in catalogue order: ties keep the first
    const std::size_t d = edit_distance(got, c);
    if (d < bd) bd = d, best = c;
  }
  return best.empty() ? std::string() : " (did you mean " + best + "?)";
}

struct Lookup {
  const Catalogue& cat;
  std::vector<std::string> rule_ids, category_ids;
  explicit Lookup(const Catalogue& c) : cat(c) {
    for (const auto& C : cat.categories) {
      category_ids.push_back(C.id);
      for (const auto& r : C.rules) rule_ids.push_back(r.id);
    }
  }
  // (category index, rule) of a rule id, or (-1, nullptr).
  std::pair<int, const RuleSpec*> rule(std::string_view id) const {
    for (std::size_t ci = 0; ci < cat.categories.size(); ++ci)
      for (const auto& r : cat.categories[ci].rules)
        if (r.id == id) return {static_cast<int>(ci), &r};
    return {-1, nullptr};
  }
};

[[noreturn]] void fail(const std::string& source, const std::string& msg) { throw std::runtime_error(source + ": " + msg); }

// "ID@REF" -> (ID, REF); an empty part on either side of '@' is an error.
std::pair<std::string, std::string> split_ref(const std::string& source, const std::string& where, const std::string& s) {
  const auto at = s.find('@');
  if (at == std::string::npos) return {s, ""};
  std::string id = s.substr(0, at), ref = s.substr(at + 1);
  if (id.empty() || ref.empty() || ref.find('@') != std::string::npos) fail(source, where + ": '" + s + "' is not ID or ID@REF");
  return {id, ref};
}

void check_keys(const std::string& source, const std::string& where, const ordered_json& obj, const std::set<std::string>& allowed) {
  for (const auto& [k, v] : obj.items()) {
    (void)v;
    if (!allowed.count(k)) {
      std::string list;
      for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
      fail(source, where + ": unknown key '" + k + "' (allowed: " + list + ")");
    }
  }
}

std::string need_string(const std::string& source, const std::string& where, const ordered_json& obj, const char* key) {
  if (!obj.contains(key) || !obj[key].is_string() || obj[key].get<std::string>().empty())
    fail(source, where + ": '" + key + "' must be a non-empty string");
  return obj[key].get<std::string>();
}

const char* json_type(const ordered_json& v) {
  if (v.is_number()) return "number";
  if (v.is_boolean()) return "boolean";
  if (v.is_string()) return "string";
  if (v.is_array()) return "list";
  if (v.is_object()) return "map";
  return "null";
}

// assert / deny: {"REF": "category" | ["category", ...]} or [{"category": ..., "ref": ...}, ...].
void parse_part_categories(const std::string& source, const Lookup& lk, const char* key, const ordered_json& v, OverrideEntry::Kind kind,
                           std::vector<OverrideEntry>& out) {
  auto add = [&](const std::string& where, const std::string& ref, const std::string& cat) {
    if (lk.cat.index_of(cat) < 0) fail(source, where + ": unknown category '" + cat + "'" + hint(cat, lk.category_ids));
    if (ref.empty() || ref.find('@') != std::string::npos) fail(source, where + ": bad reference '" + ref + "'");
    OverrideEntry e;
    e.kind = kind;
    e.category = cat;
    e.ref = ref;
    e.text = std::string(key) + " " + cat + "@" + ref;
    out.push_back(std::move(e));
  };
  if (v.is_object()) {
    for (const auto& [ref, cats] : v.items()) {
      const std::string where = std::string(key) + "." + ref;
      if (cats.is_string()) add(where, ref, cats.get<std::string>());
      else if (cats.is_array() && !cats.empty()) {
        for (const auto& c : cats) {
          if (!c.is_string()) fail(source, where + ": categories must be strings");
          add(where, ref, c.get<std::string>());
        }
      } else {
        fail(source, where + ": expected a category id or a list of them");
      }
    }
  } else if (v.is_array()) {
    for (std::size_t i = 0; i < v.size(); ++i) {
      const std::string where = std::string(key) + "[" + std::to_string(i) + "]";
      if (!v[i].is_object()) fail(source, where + ": expected {\"category\": ..., \"ref\": ...}");
      check_keys(source, where, v[i], {"category", "ref"});
      add(where, need_string(source, where, v[i], "ref"), need_string(source, where, v[i], "category"));
    }
  } else {
    fail(source, std::string(key) + ": expected a map {REF: category} or a list of {category, ref}");
  }
}

}  // namespace

Overrides parse_overrides(std::string_view json_text, const Catalogue& cat, const std::string& source) {
  ordered_json j;
  try {
    j = ordered_json::parse(json_text);
  } catch (const std::exception& e) {
    fail(source, std::string("not valid JSON: ") + e.what());
  }
  if (!j.is_object()) fail(source, "the top level must be a map");
  check_keys(source, "top level", j, {"version", "comment", "disable", "assert", "deny", "set"});
  if (j.contains("version") && (!j["version"].is_number_integer() || j["version"].get<int>() != 1)) fail(source, "version must be 1");
  if (j.contains("comment") && !j["comment"].is_string()) fail(source, "comment must be a string");
  const Lookup lk(cat);
  Overrides ov;
  ov.source = source;

  if (j.contains("disable")) {
    const auto& d = j["disable"];
    if (!d.is_array()) fail(source, "disable: expected a list of rule ids or categories (optionally ID@REF)");
    for (std::size_t i = 0; i < d.size(); ++i) {
      const std::string where = "disable[" + std::to_string(i) + "]";
      if (!d[i].is_string()) fail(source, where + ": expected a string such as \"XTAL-04\", \"USB2-04@J2\" or \"crystal\"");
      const auto [id, ref] = split_ref(source, where, d[i].get<std::string>());
      OverrideEntry e;
      e.kind = OverrideEntry::Kind::Disable;
      e.ref = ref;
      if (const auto [ci, spec] = lk.rule(id); spec) {
        e.rule = id;
        e.category = cat.categories[static_cast<std::size_t>(ci)].id;
      } else if (cat.index_of(id) >= 0) {
        e.category = id;
      } else {
        std::vector<std::string> all = lk.rule_ids;
        all.insert(all.end(), lk.category_ids.begin(), lk.category_ids.end());
        fail(source, where + ": unknown rule id or category '" + id + "'" + hint(id, all));
      }
      e.text = "disable " + d[i].get<std::string>();
      ov.entries.push_back(std::move(e));
    }
  }
  if (j.contains("assert")) parse_part_categories(source, lk, "assert", j["assert"], OverrideEntry::Kind::Assert, ov.entries);
  if (j.contains("deny")) parse_part_categories(source, lk, "deny", j["deny"], OverrideEntry::Kind::Deny, ov.entries);
  if (j.contains("set")) {
    const auto& s = j["set"];
    if (!s.is_array()) fail(source, "set: expected a list of {\"rule\": ..., \"param\": ..., \"value\": ...}");
    for (std::size_t i = 0; i < s.size(); ++i) {
      const std::string where = "set[" + std::to_string(i) + "]";
      if (!s[i].is_object()) fail(source, where + ": expected {\"rule\": ..., \"param\": ..., \"value\": ...}");
      check_keys(source, where, s[i], {"rule", "ref", "param", "value"});
      auto [id, ref] = split_ref(source, where, need_string(source, where, s[i], "rule"));
      if (s[i].contains("ref")) {
        if (!ref.empty()) fail(source, where + ": give the reference either as RULE@REF or as \"ref\", not both");
        ref = need_string(source, where, s[i], "ref");
      }
      const auto [ci, spec] = lk.rule(id);
      if (!spec) fail(source, where + ": unknown rule id '" + id + "'" + hint(id, lk.rule_ids));
      const std::string param = need_string(source, where, s[i], "param");
      if (!spec->params.contains(param)) {
        std::vector<std::string> names;
        std::string list;
        for (const auto& [k, v] : spec->params.items()) {
          (void)v;
          names.push_back(k);
          list += (list.empty() ? "" : ", ") + k;
        }
        fail(source, where + ": rule " + id + " has no parameter '" + param + "'" + hint(param, names) + (list.empty() ? " (it has none)" : " (it has: " + list + ")"));
      }
      if (!s[i].contains("value")) fail(source, where + ": 'value' is missing");
      const ordered_json& v = s[i]["value"];
      const ordered_json& old = spec->params[param];
      if (std::string(json_type(v)) != json_type(old))
        fail(source, where + ": " + id + "." + param + " is a " + json_type(old) + " (catalogue: " + old.dump() + "), got a " + json_type(v));
      if (v.is_number() && (!std::isfinite(v.get<double>()) || v.get<double>() < 0))
        fail(source, where + ": " + id + "." + param + " must be a finite, non-negative number");
      OverrideEntry e;
      e.kind = OverrideEntry::Kind::Set;
      e.rule = id;
      e.category = cat.categories[static_cast<std::size_t>(ci)].id;
      e.ref = ref;
      e.param = param;
      e.value = v;
      e.text = "set " + id + (ref.empty() ? "" : "@" + ref) + " " + param + " = " + v.dump();
      ov.entries.push_back(std::move(e));
    }
  }
  // Contradictions: the same category both asserted and denied on one part.
  for (const auto& a : ov.entries)
    for (const auto& d : ov.entries)
      if (a.kind == OverrideEntry::Kind::Assert && d.kind == OverrideEntry::Kind::Deny && a.ref == d.ref && a.category == d.category)
        fail(source, "category " + a.category + " is both asserted and denied on " + a.ref);
  return ov;
}

Overrides load_overrides_file(const std::string& path, const Catalogue& cat) {
  if (path.ends_with(".yaml") || path.ends_with(".yml"))
    throw std::runtime_error(path + ": the engine reads the override file as JSON; convert it with scripts/crules_override.py " + path +
                             " -o <file>.json (doc 15 §6.3)");
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read rules override file " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_overrides(ss.str(), cat, path);
}

void check_override_refs(const Overrides& ov, const model::Board& b) {
  std::set<std::string> refs;
  for (const auto& fp : b.footprints) refs.insert(fp.reference);
  std::vector<std::string> all(refs.begin(), refs.end());
  std::string bad;
  for (const auto& e : ov.entries)
    if (!e.ref.empty() && !refs.count(e.ref)) bad += "\n  " + e.text + ": no footprint " + e.ref + " on the board" + hint(e.ref, all);
  if (!bad.empty()) throw std::runtime_error(ov.source + ": unknown reference(s):" + bad);
}

}  // namespace tmk::crules
