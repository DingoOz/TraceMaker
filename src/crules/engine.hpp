// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "crules/impedance_rules.hpp"
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
  std::optional<ImpedancePlan> impedance;  // impedance rules: widths/gaps from the stackup (P4, report only)
  std::optional<CurrentPlan> current;      // width_for_current rules that name a current (IPC-2221, report only)
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

// Connector edge pull for the placer (doc 15 §5.2 `edge`; CONN-01, USB2-11, DISP-02), opt-in
// (tracemaker-place --edge-attraction): the anchor of every instance with a connector edge rule (mode soft/on, not
// advisory, not disabled by the user), once per footprint, never a locked part and never a pin header or socket
// (board-to-board and jumper headers, CONN-01 note). `body` holds the courtyard outline points (else pad corners),
// absolute; the placer pulls the body point nearest to the board outline toward the outline.
struct EdgeAttraction {
  int footprint = -1;
  std::vector<Point> body;
  int weight = 20;               // 2 x signal weight, like ESD-01 (doc 15 §5.2)
  double max_mm = 0;             // the rule's limit (report only)
  std::string rule, name;        // "CONN-01", "~CONN-01 J3 edge"
};
std::vector<EdgeAttraction> edge_attractions(const model::Board& b, const Catalogue& cat, const Detection& det, Mode mode);

// Keep-out rule areas (doc 15 §5.5, P2) for instances at confidence >= apply: crystal + load caps (XTAL-04),
// switching-regulator inductor (BUCK-06/BOOST-04), discrete Ethernet magnetics (ETH-05: tracks, vias and zones on
// the magnetics' side and the adjacent layer only). The polygon is the convex hull of the parts' courtyards (and the
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

// USB 2.0 D+/D- pairs to route coupled first (USB2-02, doc 15 P3): one (dp, dm) per usb2 instance whose dp and dm
// roles each bind exactly one net, in instance order, without duplicates.
std::vector<std::pair<model::NetId, model::NetId>> usb_pairs(const model::Board& b, const Catalogue& cat, const Detection& det);

// Report (doc 15 §7).
std::string report_text(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev);
nlohmann::json report_json(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev);

}  // namespace tmk::crules
