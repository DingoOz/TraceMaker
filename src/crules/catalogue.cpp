#include "crules/catalogue.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tmk::crules {

namespace {

using ojson = nlohmann::ordered_json;

Signal parse_signal(const std::string& s, const std::string& where) {
  if (s == "ref_prefix") return Signal::RefPrefix;
  if (s == "lib_id") return Signal::LibId;
  if (s == "value") return Signal::Value;
  if (s == "keywords") return Signal::Keywords;
  if (s == "pin_name") return Signal::PinName;
  if (s == "net_name") return Signal::NetName;
  if (s == "topology") return Signal::Topology;
  throw std::runtime_error(where + ": unknown detector signal '" + s + "'");
}

Severity parse_severity(const std::string& s, const std::string& where) {
  if (s == "hard") return Severity::Hard;
  if (s == "soft") return Severity::Soft;
  if (s == "advisory") return Severity::Advisory;
  throw std::runtime_error(where + ": unknown severity '" + s + "'");
}

std::vector<std::string> strings(const ojson& j, const char* key) {
  std::vector<std::string> v;
  if (!j.contains(key)) return v;
  for (const auto& e : j.at(key)) v.push_back(e.is_string() ? e.get<std::string>() : e.dump());
  return v;
}

std::string str(const ojson& j, const char* key) {
  if (!j.contains(key)) return {};
  const auto& v = j.at(key);
  return v.is_string() ? v.get<std::string>() : v.dump();
}

}  // namespace

const char* signal_name(Signal s) {
  switch (s) {
    case Signal::RefPrefix: return "ref_prefix";
    case Signal::LibId: return "lib_id";
    case Signal::Value: return "value";
    case Signal::Keywords: return "keywords";
    case Signal::PinName: return "pin_name";
    case Signal::NetName: return "net_name";
    case Signal::Topology: return "topology";
  }
  return "?";
}

const char* severity_name(Severity s) {
  switch (s) {
    case Severity::Hard: return "hard";
    case Severity::Soft: return "soft";
    case Severity::Advisory: return "advisory";
  }
  return "?";
}

bool RuleSpec::enforced_in(std::string_view stage) const {
  for (const auto& e : enforce)
    if (e == stage) return true;
  return false;
}

bool Category::has_signal(Signal s) const {
  for (const auto& d : detect)
    if (d.signal == s) return true;
  return false;
}

int Catalogue::index_of(std::string_view id) const {
  for (std::size_t i = 0; i < categories.size(); ++i)
    if (categories[i].id == id) return static_cast<int>(i);
  return -1;
}

std::size_t Catalogue::rule_count() const {
  std::size_t n = 0;
  for (const auto& c : categories) n += c.rules.size();
  return n;
}

Catalogue parse_catalogue(std::string_view json_text) {
  const ojson j = ojson::parse(json_text);
  Catalogue cat;
  cat.version = j.value("version", 0);
  if (j.contains("defaults") && j["defaults"].contains("thresholds")) {
    const auto& t = j["defaults"]["thresholds"];
    cat.apply = t.value("apply", cat.apply);
    cat.suggest = t.value("suggest", cat.suggest);
  }
  if (j.contains("defaults") && j["defaults"].contains("prop_delay_ps_per_mm")) {
    const auto& t = j["defaults"]["prop_delay_ps_per_mm"];
    cat.prop_delay_outer = t.value("outer", cat.prop_delay_outer);
    cat.prop_delay_inner = t.value("inner", cat.prop_delay_inner);
  }
  for (const auto& jc : j.at("categories")) {
    Category c;
    c.id = jc.at("id").get<std::string>();
    c.name = str(jc, "name");
    c.prefix = str(jc, "prefix");
    for (const auto& jd : jc.at("detect")) {
      Detector d;
      const std::string where = "category " + c.id;
      d.signal = parse_signal(jd.at("signal").get<std::string>(), where);
      d.pattern = jd.at("pattern").get<std::string>();
      d.weight = jd.at("weight").get<int>();
      if (d.weight < 0 || d.weight > 100) throw std::runtime_error(where + ": detector weight outside 0..100");
      if (d.signal != Signal::Topology) {
        try {
          d.re = std::regex(d.pattern, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        } catch (const std::regex_error& e) {
          throw std::runtime_error(where + ": bad pattern '" + d.pattern + "': " + e.what());
        }
      }
      c.detect.push_back(std::move(d));
    }
    if (jc.contains("roles"))
      for (const auto& [k, v] : jc["roles"].items()) c.roles.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
    for (const auto& jr : jc.at("rules")) {
      RuleSpec r;
      r.id = jr.at("id").get<std::string>();
      r.kind = jr.at("kind").get<std::string>();
      r.text = str(jr, "rule");
      r.evidence = str(jr, "evidence");
      r.note = str(jr, "note");
      r.applies_to = strings(jr, "applies_to");
      r.enforce = strings(jr, "enforce");
      r.sources = strings(jr, "sources");
      r.params = jr.contains("params") ? jr["params"] : ojson::object();
      r.severity = parse_severity(jr.at("severity").get<std::string>(), "rule " + r.id);
      c.rules.push_back(std::move(r));
    }
    cat.categories.push_back(std::move(c));
  }
  return cat;
}

Catalogue load_catalogue_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read catalogue " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_catalogue(ss.str());
}

const Catalogue& builtin_catalogue() {
  static const Catalogue cat = parse_catalogue(builtin_catalogue_json());
  return cat;
}

}  // namespace tmk::crules
