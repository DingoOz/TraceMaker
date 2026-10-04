#pragma once
// Component-rule catalogue (design doc 15 §6.1): categories of components with their detectors, roles and
// layout rules. The source is docs/component_rules.yaml; scripts/crules_catalogue.py converts it to
// src/crules/catalogue.json, which the build embeds (the engine has no YAML parser), and a ctest keeps the
// two identical.
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace tmk::crules {

// What a detector looks at (doc 15 §3.1).
enum class Signal : std::uint8_t { RefPrefix, LibId, Value, Keywords, PinName, NetName, Topology };
enum class Severity : std::uint8_t { Hard, Soft, Advisory };

const char* signal_name(Signal s);
const char* severity_name(Severity s);

struct Detector {
  Signal signal = Signal::RefPrefix;
  std::string pattern;           // ECMAScript regex (case-insensitive), or a topology name for Signal::Topology
  int weight = 0;                // 0..100; confidence = min(100, sum of matching weights)
  std::regex re;                 // compiled `pattern` (unused for topology)
};

struct RuleSpec {
  std::string id, kind, text, evidence, note;
  std::vector<std::string> applies_to, enforce, sources;
  nlohmann::ordered_json params;
  Severity severity = Severity::Soft;
  bool enforced_in(std::string_view stage) const;
};

struct Category {
  std::string id, name, prefix;
  std::vector<Detector> detect;
  std::vector<std::pair<std::string, std::string>> roles;  // name -> description, catalogue order
  std::vector<RuleSpec> rules;
  bool has_signal(Signal s) const;
};

struct Catalogue {
  int version = 0;
  int apply = 70, suggest = 40;  // confidence thresholds (doc 15 §3.2)
  std::vector<Category> categories;
  int index_of(std::string_view id) const;  // -1 if absent
  std::size_t rule_count() const;
};

// Parses the catalogue JSON. Throws std::runtime_error naming the offending entry on schema errors (an
// unknown signal, a weight outside 0..100, a bad regex, a missing rule id/kind/severity).
Catalogue parse_catalogue(std::string_view json_text);
Catalogue load_catalogue_file(const std::string& path);
// The catalogue embedded at build time (src/crules/catalogue.json), parsed once.
const Catalogue& builtin_catalogue();
std::string_view builtin_catalogue_json();

}  // namespace tmk::crules
