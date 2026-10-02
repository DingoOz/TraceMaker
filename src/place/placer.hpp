#pragma once
// The placement pipeline (design doc 04 §3): `full` runs A (quadratic B2B) → B (SimPL spreading) →
// C (rotations) → D (legalisation) → E (annealing); `refine` starts from the current placement and runs
// D (only for illegal parts) → E. Both report the exact lower bounds of doc 04 §1 (L4) and what was achieved.
#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "place/anneal.hpp"
#include "place/legality.hpp"

namespace tmk::place {

struct PlaceOptions {
  std::string mode = "full";     // full | refine
  std::uint64_t seed = 1;
  int threads = 0;               // 0 = min(hardware, 16)
  int runs = 0;                  // annealing runs (0 = threads)
  double effort = 1.0;
  double alpha_cross_mm = 2.0;
  double time_limit_s = 0;       // wall-time stop for the annealer (0 = none)
  bool verbose = false;
};

struct Metrics {
  std::int64_t whpwl = 0;        // Σ weight·HPWL (signal nets weight 10, power nets 1), nm
  std::int64_t hpwl = 0;         // Σ HPWL over all nets, nm
  std::int64_t crossings = 0;    // MST airwire crossings between different signal nets
  int overlaps = 0, outside = 0; // violations involving movable parts
  int fixed_overlaps = 0, fixed_outside = 0;
  // Relative to a reference (the input) placement: violations where at least one involved movable part was
  // moved. Pairs whose parts both stayed put existed in the input and are not ours.
  int new_overlaps = 0, new_outside = 0;
  std::vector<std::string> conflicts;  // "A/B" pairs and "A/outline" (movable-involved)
};

struct PlaceReport {
  std::string mode;
  int parts = 0, movable = 0, nets = 0, pins = 0;
  Metrics before, after;
  std::int64_t lb_fixed_rot = 0;   // L4 bound with the input's rotations
  std::int64_t lb_any_rot = 0;     // L4 bound valid for any rotations (bounds the final placement)
  std::int64_t quadratic_whpwl = -1;  // weighted HPWL of the B2B quadratic optimum (A), full mode
  double overflow_quadratic = 0, overflow_spread = 0;
  std::array<double, 2> utilisation{0, 0};  // movable demand / free area per side
  int spread_iterations = 0, rotation_changes = 0;
  int legalise_failed = 0, legalise_placed = 0;
  std::vector<std::string> legalise_failures;  // references of the parts that found no position
  double legalise_mean_disp_mm = 0, legalise_max_disp_mm = 0;
  std::int64_t legal_start_whpwl = 0;  // weighted HPWL entering the annealer
  AnnealResult anneal;
  double seconds_total = 0;
  std::vector<std::pair<std::string, double>> stage_seconds;
  std::vector<std::string> notes, log;
  bool legal = false;              // no new violation (relative to the input) in the result
};

Metrics measure(const Problem& p, const Placement& pl, const Placement* reference = nullptr);
PlaceReport place(const Problem& p, Placement& pl, const PlaceOptions& o);
nlohmann::json report_json(const Problem& p, const PlaceReport& r);

}  // namespace tmk::place
