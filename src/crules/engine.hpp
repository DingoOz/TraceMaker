#pragma once
// From detected instances to effective rules and engine inputs (design doc 15 §3.5, §5.5, §7):
//   evaluate  -> every catalogue rule of every instance, tagged applied / satisfied-by-board / not-applied(reason)
//                / advisory, with a measured value where TraceMaker can measure it;
//   compile   -> placement pseudo-nets (pad pairs), keep-out rule areas for the router and DRC, and a sidecar
//                .kicad_dru with the generated custom rules;
//   report    -> the text and JSON the user sees.
// Everything is deterministic: instances in catalogue/reference order, rules in catalogue order.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crules/catalogue.hpp"
#include "crules/detect.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::crules {

// --component-rules (doc 15 §3.5): off = nothing; report = detect and check only; soft = rules as costs, no hard
// constraints; on = catalogue severities (generated keep-outs become router/DRC rule areas).
enum class Mode : std::uint8_t { Off, Report, Soft, On };
Mode parse_mode(std::string_view s);  // throws std::invalid_argument
const char* mode_name(Mode m);

enum class Status : std::uint8_t { Applied, SatisfiedByBoard, NotApplied, Advisory };
const char* status_name(Status s);

struct Measure {
  double value_mm = 0, limit_mm = 0;
  bool met = false;
  std::string what;              // how it was measured
};

struct EffectiveRule {
  int instance = -1;             // index into Detection::instances
  const RuleSpec* spec = nullptr;
  Severity severity = Severity::Soft;  // after confidence demotion (doc 15 §3.2)
  Status status = Status::NotApplied;
  std::string detail;            // what was applied, or why not
  std::optional<Measure> measure;
};

struct Evaluation {
  Mode mode = Mode::Report;
  std::vector<EffectiveRule> rules;
};

// A proximity rule bound to pads (doc 15 §4 `proximity`): each pair is (pad of the part that should move close,
// pad it should be close to). Shared by the measurement and the placement pseudo-nets so both agree.
struct ProximityPairs {
  const RuleSpec* spec = nullptr;
  int instance = -1;
  std::vector<std::pair<int, int>> pairs;
  double limit_mm = 0;
  int weight = 10;               // placement pseudo-net weight (kSignalWeight; x2 for ESD to connector, doc 15 §5.2)
  std::string unbound;           // non-empty: a role it needs is missing
};
std::vector<ProximityPairs> proximity_pairs(const model::Board& b, const Catalogue& cat, const Detection& det);

Evaluation evaluate(const model::Board& b, const model::DesignRules* rules, const Catalogue& cat, const Detection& det, Mode mode);

// Placement pseudo-nets for every applicable proximity rule (mode soft or on; confidence >= suggest). A part is
// tied by the first rule that claims it, in this priority: crystal, oscillator, ESD/protection, regulator, then
// decoupling; later rules skip it.
struct PlacementAffinity {
  int pad_a = -1, pad_b = -1, weight = 10;
  std::string name, rule;
};
std::vector<PlacementAffinity> placement_affinities(const model::Board& b, const Catalogue& cat, const Detection& det, Mode mode);

// Keep-out rule areas (doc 15 §5.5, P2) for instances at confidence >= apply: crystal + load caps (XTAL-04),
// switching-regulator inductor (BUCK-06/BOOST-04). The polygon is the convex hull of the parts' courtyards (and the
// IC's oscillator pins for a crystal) grown by the rule margin, on the copper layers where none of those parts has
// a pad, so the parts' own nets still reach their pads; tracks only (vias and zones stay allowed: ground stitching
// under a crystal is good practice). Zones are named "tmk:<rule>:<ref>".
struct GeneratedKeepout {
  model::Zone zone;
  std::string rule, ref;
  int instance = -1;
};
std::vector<GeneratedKeepout> generate_keepouts(const model::Board& b, const Catalogue& cat, const Detection& det,
                                                std::vector<std::string>* not_generated = nullptr);
// The same intent as KiCad custom rules with net exemptions (which rule areas cannot express), for the user to
// review or merge: "<output>.tracemaker.kicad_dru". Never written into the user's own files (rule 8).
std::string dru_sidecar(const model::Board& b, const Catalogue& cat, const Detection& det);

// Report (doc 15 §7).
std::string report_text(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev);
nlohmann::json report_json(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev);

}  // namespace tmk::crules
