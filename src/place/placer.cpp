#include "place/placer.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "place/global.hpp"
#include "place/legalize.hpp"
#include "place/lower_bound.hpp"
#include "place/wirelength.hpp"

namespace tmk::place {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
double mm(std::int64_t nm) { return static_cast<double>(nm) / 1e6; }
}  // namespace

Metrics measure(const Problem& p, const Placement& pl, const Placement* reference) {
  Metrics m;
  m.whpwl = weighted_hpwl(p, pl);
  m.hpwl = total_hpwl(p, pl);
  m.crossings = count_crossings(p, pl);
  m.side_vias = side_vias(p, pl, false);
  for (std::size_t i = 0; i < p.parts.size(); ++i) m.flipped += flipped(pl.rot[i]) ? 1 : 0;
  const Violations v = check_all(p, pl);
  m.overlaps = v.overlaps;
  m.outside = v.outside;
  m.fixed_overlaps = v.fixed_overlaps;
  m.fixed_outside = v.fixed_outside;
  auto moved = [&](int i) {
    return !reference || pl.pos[z(i)] != reference->pos[z(i)] || pl.rot[z(i)] != reference->rot[z(i)];
  };
  for (const auto& [a, b] : v.pairs) {
    m.conflicts.push_back(p.parts[z(a)].ref + "/" + p.parts[z(b)].ref);
    if (moved(a) || moved(b)) ++m.new_overlaps;
  }
  for (int a : v.outside_parts) {
    m.conflicts.push_back(p.parts[z(a)].ref + "/outline");
    if (moved(a)) ++m.new_outside;
  }
  return m;
}

PlaceReport place(const Problem& p, Placement& pl, const PlaceOptions& o) {
  PlaceReport r;
  const auto t_start = Clock::now();
  r.mode = o.mode;
  r.parts = static_cast<int>(p.parts.size());
  r.movable = p.movable_count();
  r.nets = static_cast<int>(p.nets.size());
  r.pins = static_cast<int>(p.pins.size());
  r.notes = p.notes;
  r.utilisation = utilisation(p);
  auto stage = [&](const char* name, Clock::time_point t) { r.stage_seconds.emplace_back(name, since(t)); };

  auto t = Clock::now();
  const Placement input = pl;
  auto emit = [&](const char* st, const Placement& x, double when = -1) {
    if (o.trace) o.trace(st, x, when < 0 ? trace_now() : when);
  };
  emit("input", pl);
  r.before = measure(p, pl);
  r.lb_fixed_rot = hpwl_lower_bound(p, pl, RotationModel::Fixed);
  r.lb_any_rot = hpwl_lower_bound(p, pl, RotationModel::Any);
  stage("measure + bounds", t);

  const bool full = o.mode == "full";
  if (full && r.movable > 0) {
    // (A) quadratic optimum from scratch: every movable part starts at the board centre.
    t = Clock::now();
    const Point centre{(p.region.x0 + p.region.x1) / 2, (p.region.y0 + p.region.y1) / 2};
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (p.parts[i].movable) pl.pos[i] = centre - Point{(p.parts[i].geom[0].body.x0 + p.parts[i].geom[0].body.x1) / 2,
                                                         (p.parts[i].geom[0].body.y0 + p.parts[i].geom[0].body.y1) / 2};
    quadratic_place(p, pl, 10);
    emit("quadratic", pl);
    r.quadratic_whpwl = weighted_hpwl(p, pl);
    stage("A quadratic", t);
    // (B) SimPL spreading.
    t = Clock::now();
    std::vector<Placement> spread_trace;
    const SpreadStats ss = spread(p, pl, 40, 0.10, o.trace ? &spread_trace : nullptr);
    for (const auto& x : spread_trace) emit("spreading", x);
    r.overflow_quadratic = ss.overflow_start;
    r.overflow_spread = density_overflow(p, pl);
    r.spread_iterations = ss.iterations;
    if (o.verbose) r.log.insert(r.log.end(), ss.log.begin(), ss.log.end());
    stage("B spreading", t);
    // (C) rotations.
    t = Clock::now();
    r.rotation_changes = optimise_rotations(p, pl);
    emit("rotations", pl);
    stage("C rotations", t);
  }
  // (D) legalisation.
  t = Clock::now();
  // Full mode: if some parts found no place, retry with the failures first (they are usually the big parts
  // whose room was taken by smaller ones), up to four times.
  const Placement before_legal = pl;
  LegaliseStats ls = legalise(p, pl, !full);
  std::vector<int> first;
  std::vector<int> last_failed = ls.failed_parts;
  for (int attempt = 1; full && ls.failed > 0 && attempt < 5; ++attempt) {
    const std::size_t before = first.size();
    for (int f : last_failed)
      if (std::find(first.begin(), first.end(), f) == first.end()) first.push_back(f);
    if (first.size() == before) break;  // the same order again would give the same result
    Placement retry = before_legal;
    LegaliseStats lr = legalise(p, retry, false, 0, &first);
    last_failed = lr.failed_parts;
    r.notes.push_back("legalisation attempt " + std::to_string(attempt + 1) + " (" + std::to_string(first.size()) +
                      " earlier failures first): " + std::to_string(lr.failed) + " failed");
    if (lr.failed < ls.failed) {
      ls = lr;
      pl = retry;
    }
  }
  r.legalise_failed = ls.failed;
  r.legalise_failures = ls.failures;
  r.legalise_placed = ls.placed;
  r.legalise_mean_disp_mm = ls.mean_disp_mm;
  r.legalise_max_disp_mm = ls.max_disp_mm;
  for (const auto& f : ls.failures) r.notes.push_back("legaliser found no position for " + f);
  if (ls.raster_disagree) r.notes.push_back("raster/exact disagreement x" + std::to_string(ls.raster_disagree));
  stage("D legalisation", t);
  emit("legalised", pl);
  r.legal_start_whpwl = weighted_hpwl(p, pl);

  // (E) annealing (needs a legal start for the movable parts).
  t = Clock::now();
  AnnealOptions ao;
  ao.seed = o.seed;
  ao.threads = o.threads > 0 ? o.threads : static_cast<int>(std::min(16u, std::max(1u, std::thread::hardware_concurrency())));
  ao.runs = o.runs > 0 ? o.runs : ao.threads;
  ao.effort = o.effort;
  ao.alpha_cross_mm = o.alpha_cross_mm;
  ao.time_limit_s = o.time_limit_s;
  ao.refine = !full;
  ao.tempering = o.tempering;
  ao.lns_rate = o.lns_rate;
  ao.lns_window = o.lns_window;
  ao.beta_congestion = o.beta_congestion;
  ao.congestion = o.congestion;
  ao.focus = o.focus;
  ao.flip = o.flip;
  ao.via_mm = o.via_mm;
  ao.flip_rate = o.flip_rate;
  if (o.trace) {  // about 120 frames of the winning run
    const std::uint64_t moves = std::max<std::uint64_t>(20'000, static_cast<std::uint64_t>(o.effort * 4000.0 * r.movable));
    ao.trace_every = std::max<std::uint64_t>(1, moves / 120);
  }
  if (r.movable > 0) {
    r.anneal = anneal(p, pl, ao);
    for (std::size_t i = 0; i < r.anneal.trace.size(); ++i) emit("annealing", r.anneal.trace[i], r.anneal.trace_t[i]);
    r.anneal.trace.clear();
    r.anneal.trace_t.clear();
    pl = r.anneal.pl;
  }
  stage("E annealing", t);
  if (r.movable > 0 && o.lns_polish > 0) {
    t = Clock::now();
    const LnsResult lr = lns_improve(p, pl, ao, o.lns_polish);
    pl = lr.pl;
    emit("lns", pl);
    r.lns_tried = lr.tried;
    r.lns_improved = lr.improved;
    stage("E LNS polish", t);
  }

  t = Clock::now();
  r.after = measure(p, pl, &input);
  r.legal = r.after.new_overlaps == 0 && r.after.new_outside == 0;
  emit("placed", pl);
  stage("final check", t);
  r.seconds_total = since(t_start);
  return r;
}

nlohmann::json report_json(const Problem& p, const PlaceReport& r) {
  using nlohmann::json;
  auto metrics = [](const Metrics& m) {
    return json{{"weighted_hpwl_mm", mm(m.whpwl) / kSignalWeight}, {"hpwl_mm", mm(m.hpwl)},     {"crossings", m.crossings},
                {"overlaps", m.overlaps},                          {"outside", m.outside},      {"fixed_overlaps", m.fixed_overlaps},
                {"fixed_outside", m.fixed_outside}, {"new_overlaps", m.new_overlaps}, {"new_outside", m.new_outside},
                {"flipped", m.flipped}, {"side_vias", m.side_vias}, {"conflicts", m.conflicts}};
  };
  json j;
  j["mode"] = r.mode;
  j["parts"] = r.parts;
  j["movable"] = r.movable;
  j["nets"] = r.nets;
  j["pins"] = r.pins;
  j["before"] = metrics(r.before);
  j["after"] = metrics(r.after);
  j["legal"] = r.legal;
  // Weighted HPWL is reported in "signal-mm" (divided by the signal weight 10, so power nets count 0.1).
  j["bounds"] = {{"lb_fixed_rotation_mm", mm(r.lb_fixed_rot) / kSignalWeight},
                 {"lb_any_rotation_mm", mm(r.lb_any_rot) / kSignalWeight},
                 {"quadratic_b2b_mm", r.quadratic_whpwl < 0 ? json(nullptr) : json(mm(r.quadratic_whpwl) / kSignalWeight)},
                 {"gap_final_over_lb_any", r.lb_any_rot > 0 ? static_cast<double>(r.after.whpwl) / static_cast<double>(r.lb_any_rot) : 0.0}};
  j["utilisation"] = {r.utilisation[0], r.utilisation[1]};
  j["global"] = {{"overflow_quadratic", r.overflow_quadratic}, {"overflow_spread", r.overflow_spread},
                 {"spread_iterations", r.spread_iterations},   {"rotation_changes", r.rotation_changes}};
  j["legalise"] = {{"placed", r.legalise_placed}, {"failed", r.legalise_failed}, {"mean_disp_mm", r.legalise_mean_disp_mm},
                   {"max_disp_mm", r.legalise_max_disp_mm}, {"weighted_hpwl_mm", mm(r.legal_start_whpwl) / kSignalWeight}};
  j["anneal"] = {{"best_run", r.anneal.best_run}, {"moves", r.anneal.moves}, {"accepted", r.anneal.accepted},
                 {"rejected_illegal", r.anneal.illegal}, {"time_limited", r.anneal.time_limited}, {"run_costs", r.anneal.run_costs},
                 {"exchanges_tried", r.anneal.exchanges_tried}, {"exchanges_accepted", r.anneal.exchanges_accepted},
                 {"lns_tried", r.anneal.lns_tried + r.lns_tried}, {"lns_improved", r.anneal.lns_improved + r.lns_improved},
                 {"rudy_overflow_mm", mm(r.anneal.overflow)}};
  json st = json::object();
  for (const auto& [k, v] : r.stage_seconds) st[k] = v;
  j["seconds"] = st;
  j["seconds_total"] = r.seconds_total;
  j["notes"] = r.notes;
  json fixed = json::array();
  for (const auto& pt : p.parts)
    if (!pt.movable) fixed.push_back({{"ref", pt.ref}, {"reason", pt.fixed_reason}});
  j["fixed_parts"] = fixed;
  j["claims"] = {
      {"L1", r.quadratic_whpwl < 0 ? "not run (refine mode)"
                                   : "B2B quadratic solved to CG tolerance: the global optimum of the quadratic model at its last "
                                     "linearisation (an approximation of the HPWL relaxation, not a bound)"},
      {"L2", "not achieved: legalisation is greedy minimum-displacement per part (spiral search), not the LP"},
      {"L3", "rotation of each part is the exact HPWL optimum with its neighbours fixed after stage C (full mode); window "
             "CP-SAT and Hungarian slot assignment are not implemented"},
      {"L4", "exact: lb_any_rotation is the LP optimum (min-cost flow, integer) of weighted HPWL without overlap/outline "
             "constraints, valid for every rotation (and, for parts that may flip, side) of the movable parts; every legal placement has weighted HPWL >= it"}};
  return j;
}

}  // namespace tmk::place
