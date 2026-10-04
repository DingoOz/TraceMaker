#pragma once
// Impedance and current rules on a real board (design doc 15 §5.3-5.4, P4): the board stackup gives, per routing
// layer, the line structure and the dielectric to the nearest reference layer; the closed forms of impedance.hpp
// turn a rule's target (z0_ohm / zdiff_ohm) into a width and gap. Report-only: nothing here changes routing.
//
// Assumption (dev/assumptions-m13.md, P4): the copper layer adjacent to a routing layer is its reference plane.
// Outer layers are microstrip over the next copper layer; inner layers are stripline between the copper layers
// above and below. A zone on the reference layer is reported as confirmation; without one the report says the
// plane is assumed. Boards without a stackup get nothing computed (doc 15 §3.6; CLAUDE.md rule 6).
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crules/catalogue.hpp"
#include "crules/impedance.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::crules {

// One copper layer's line geometry from the stackup.
struct LayerGeometry {
  int copper = -1;
  std::string layer;
  bool routing = true;                 // false for KiCad "power" (plane) layers
  imp::Structure structure = imp::Structure::Microstrip;  // outer: microstrip; inner: stripline
  int ref_a = -1, ref_b = -1;          // reference copper layers (microstrip: ref_a only; stripline: above, below)
  std::string ref_a_note, ref_b_note;  // "zone GND" or "no zone: assumed plane"
  Coord h1 = 0, h2 = 0;                // dielectric to ref_a (microstrip) / to the planes above and below (stripline)
  Coord t = 0;                         // copper thickness of this layer
  double er = 0;                       // dielectric permittivity (stripline: series combination of both sides)
  bool ok = false;
  std::string why;                     // when !ok
};
std::vector<LayerGeometry> stackup_geometry(const model::Board& b);

// The width (and gap) that meets one target on one layer.
struct LineSolution {
  std::string layer, ref;              // "F.Cu", "In1.Cu (zone GND)"
  imp::Structure structure = imp::Structure::Microstrip;
  bool differential = false;
  double target = 0;                   // ohm
  bool ok = false;
  std::string why;                     // when !ok
  Coord width = 0, gap = 0;            // gap: pair gap (differential) or coplanar ground gap (GCPW); 0 otherwise
  double z = 0;                        // achieved impedance at the integer-nm solution
  double eps_eff = 0, ps_per_mm = 0;   // differential: odd mode
  double error_pct = 0;                // stated formula error
  Coord h = 0, h2 = 0;                 // dielectric to the reference (stripline: above, below)
  double er = 0;
  std::string note;                    // e.g. "gap raised: width at the board minimum"
};

struct ImpedancePlan {
  bool computed = false;               // false: `why` says why (no stackup, no target, ...)
  std::string why;
  double z0 = 0, zdiff = 0, tol_pct = 0;
  std::string structure;               // rule parameter, "" = microstrip/stripline by layer
  Coord min_width = 0, pair_gap = 0, coplanar_gap = 0;  // the bounds used, and where they came from
  std::string bounds_note;
  std::vector<LineSolution> lines;
};

// Board bounds used for solving: the narrowest width and the pair / coplanar gap (see impedance_rules.cpp).
ImpedancePlan plan_impedance(const model::Board& b, const model::DesignRules* rules, const RuleSpec& r);
std::string impedance_text(const ImpedancePlan& p);
// Differential impedance of an existing width/gap on a copper layer (to compare a board net class with the target);
// < 0 when the layer has no complete stackup geometry.
double pair_impedance(const model::Board& b, int copper, Coord width, Coord gap);
nlohmann::json impedance_json(const ImpedancePlan& p);

// Width for a rule that names a current (doc 15 §5.4, IPC-2221).
struct CurrentPlan {
  bool computed = false;
  std::string why;
  double amps = 0, delta_t_c = 0;
  Coord outer_copper = 0, inner_copper = 0;  // inner 0: no inner layers
  Coord outer_width = 0, inner_width = 0;
  bool copper_assumed = false;         // no stackup: 35 um (1 oz) assumed
};
CurrentPlan plan_current(const model::Board& b, const RuleSpec& r);
std::string current_text(const CurrentPlan& p);
nlohmann::json current_json(const CurrentPlan& p);

// Propagation delay per copper layer used to turn ps skew budgets into mm (doc 15 §4): from the stackup (eps_eff
// of a 50 ohm line on outer layers, er on inner layers) or the catalogue's FR-4 defaults.
struct LayerDelay {
  std::string layer;
  double ps_per_mm = 0;
  bool from_stackup = false;
};
std::vector<LayerDelay> prop_delays(const model::Board& b, const Catalogue& cat);
// One line for the report header: the stackup summary and the delays, or why defaults are used.
std::string stackup_summary(const model::Board& b, const Catalogue& cat);
nlohmann::json stackup_json(const model::Board& b, const Catalogue& cat);

}  // namespace tmk::crules
