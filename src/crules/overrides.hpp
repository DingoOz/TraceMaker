// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The user override file (design doc 15 §3.5 level 1, §6.3): the user disables rules, forces or forbids a
// category on a part, or changes a rule parameter. The engine reads JSON (it has no YAML parser, like the
// catalogue); the YAML form of doc 15 §6.3 maps 1:1 to it and scripts/crules_override.py converts it.
//
//   {
//     "version": 1,
//     "disable": ["USB2-04@J2", "XTAL-04", "mounting_hole", "crystal@Y2"],
//     "assert":  {"J5": "usb2"},                    // or [{"category": "usb2", "ref": "J5"}]
//     "deny":    {"U7": "buck"},                    // or [{"category": "buck", "ref": "U7"}]
//     "set":     [{"rule": "XTAL-02", "param": "max_mm", "value": 5},
//                 {"rule": "XTAL-01@Y1", "param": "max_mm", "value": 8}]
//   }
//
// Everything is validated against the catalogue when the file is read: unknown keys, rule ids, categories,
// parameters and value types are errors, never ignored (a typo must not silently drop an override). References
// are validated against the board by check_override_refs (detect() calls it).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crules/catalogue.hpp"
#include "model/board.hpp"

namespace tmk::crules {

struct OverrideEntry {
  enum class Kind : std::uint8_t { Disable, Assert, Deny, Set };
  Kind kind = Kind::Disable;
  std::string rule;               // Disable (one rule) and Set: the rule id
  std::string category;           // the category id (for rule entries: the rule's category)
  std::string ref;                // anchor reference; empty = every instance (Disable, Set)
  std::string param;              // Set: parameter name
  nlohmann::ordered_json value;   // Set: new value
  std::string text;               // the entry as the report shows it, e.g. "disable XTAL-04@Y1"
};

struct Overrides {
  std::string source;                 // file name (messages and report)
  std::vector<OverrideEntry> entries; // file order: disable, assert, deny, set
  bool empty() const { return entries.empty(); }
};

// Parses and validates an override file. Throws std::runtime_error with a message that names the source and the
// offending entry.
Overrides parse_overrides(std::string_view json_text, const Catalogue& cat, const std::string& source = "rules override");
// Reads a JSON file; a .yaml/.yml file is refused with the conversion command.
Overrides load_overrides_file(const std::string& path, const Catalogue& cat);
// Every reference must name a footprint of the board (throws otherwise, listing the unknown ones).
void check_override_refs(const Overrides& ov, const model::Board& b);

}  // namespace tmk::crules
