// SPDX-License-Identifier: GPL-3.0-or-later
// tracemaker-place: component placement for a .kicad_pcb (design doc 04).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "crules/engine.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "sexpr/sexpr.hpp"
#include "io/kicad/project_reader.hpp"
#include "place/legality.hpp"
#include "place/lower_bound.hpp"
#include "place/placer.hpp"
#include "place/routable.hpp"
#include "place/wirelength.hpp"
#include "route/router.hpp"

namespace {

using namespace tmk;

// --record: the placements along the chain that produced the result (video timelapse), each with the time it
// existed, and a log of what the job was doing when (building a candidate, routing one, ...). Frames are kept per
// candidate label; the chain is the sequence of incumbents, so only the stages that led to the output are written.
struct PlaceRecorder {
  struct Frame {
    std::string stage;
    place::Placement pl;
    double t;
  };
  double t0 = place::trace_now();
  std::map<std::string, std::vector<Frame>> frames;
  std::vector<std::string> chain;
  std::vector<std::pair<double, std::string>> phases;

  void add(const std::string& key, const place::Placement& pl, double t) {
    const auto bar = key.find('|');
    const std::string label = key.substr(0, bar), stage = bar == std::string::npos ? key : key.substr(bar + 1);
    auto& v = frames[label];
    if (stage == "input") v.clear();  // a fallback re-run of the same candidate starts over
    v.push_back({stage, pl, t});
  }
  void phase(const std::string& text) { phases.emplace_back(place::trace_now(), text); }
  void write(const std::string& path, const place::Problem& p, const place::Placement& input, const place::Placement& result,
             const std::string& board) const {
    std::ofstream f(path);
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& pt : p.parts)
      parts.push_back({{"ref", pt.ref}, {"x0", pt.pos0.x}, {"y0", pt.pos0.y}, {"angle0", pt.angle0}, {"side", pt.side}, {"movable", pt.movable}});
    const double t_end = place::trace_now() - t0;
    f << nlohmann::json{{"type", "place_header"}, {"board", board}, {"parts", parts}, {"chain", chain}, {"seconds", t_end}}.dump() << "\n";
    for (const auto& [t, text] : phases) f << nlohmann::json{{"type", "place_phase"}, {"t", t - t0}, {"text", text}}.dump() << "\n";
    auto frame = [&](const std::string& label, const std::string& stage, const place::Placement& pl, double t) {
      nlohmann::json pos = nlohmann::json::array();
      for (std::size_t i = 0; i < p.parts.size(); ++i) pos.push_back({pl.pos[i].x, pl.pos[i].y, pl.rot[i]});
      f << nlohmann::json{{"type", "place_frame"}, {"label", label}, {"stage", stage}, {"t", std::max(0.0, t - t0)},
                          {"hpwl", place::total_hpwl(p, pl)}, {"pos", pos}}.dump()
        << "\n";
    };
    frame("input", "input", input, t0);
    for (const auto& label : chain) {
      const auto it = frames.find(label);
      if (it == frames.end()) continue;
      for (const auto& fr : it->second)
        if (fr.stage != "input" || label != chain.front()) frame(label, fr.stage, fr.pl, fr.t);
    }
    frame("result", "result", result, place::trace_now());
  }
};

struct Placed {
  place::Problem p;
  place::Placement pl;
  place::PlaceReport r;
};

// full or refine with the full-mode fallbacks (clearance 0, keeping unplaceable parts, refine as last resort).
Placed place_with_fallbacks_one(const io::LoadedBoard& lb, const model::DesignRules& rules, const std::string& in, place::ExtractOptions eo,
                                const place::PlaceOptions& o, bool no_fallback);

Placed place_with_fallbacks(const io::LoadedBoard& lb, const model::DesignRules& rules, const std::string& in, place::ExtractOptions eo,
                            const place::PlaceOptions& o, bool no_fallback) {
  if (!(eo.crules_two_stage && o.mode == "full" && !eo.affinities.empty())) return place_with_fallbacks_one(lb, rules, in, eo, o, no_fallback);
  // Stage 1: decoupling ties only (D25 and the component rules' own DEC-* ties).
  place::ExtractOptions e1 = eo;
  std::erase_if(e1.affinities, [](const auto& a) { return !a.name.starts_with("~DEC-"); });
  Placed s1 = place_with_fallbacks_one(lb, rules, in, e1, o, no_fallback);
  // Stage 2: the same problem with the component-rule pulls, starting from stage 1, decoupling capacitors locked.
  place::ExtractOptions e2 = eo;
  e2.default_clearance = s1.p.clearance;  // whatever spacing stage 1 settled on
  Placed x;
  x.p = place::extract(lb.board, rules, in, e2);
  if (x.p.parts.size() != s1.p.parts.size()) return s1;
  x.pl = s1.pl;
  int locked = 0;
  for (const auto& n : x.p.nets)
    if (n.affinity && (n.name.starts_with("~decap ") || n.name.starts_with("~DEC-")) && !n.pins.empty()) {
      // Both ends: the capacitor and the IC it decouples (an IC pulled towards its crystal would leave its
      // capacitors behind).
      for (int pin : n.pins) {
        auto& pt = x.p.parts[static_cast<std::size_t>(x.p.pins[static_cast<std::size_t>(pin)].part)];
        if (pt.movable) {
          pt.movable = false;
          pt.fixed_reason = "decoupling capacitor or its IC, placed in stage 1";
          ++locked;
        }
      }
    }
  for (std::size_t i = 0; i < x.p.parts.size(); ++i)
    if (!s1.p.parts[i].movable) x.p.parts[i].movable = false;  // stage-1 fallbacks stay
  place::PlaceOptions o2 = o;
  o2.mode = "refine";
  x.r = place::place(x.p, x.pl, o2);
  if (!x.r.legal) return s1;
  x.r.notes.insert(x.r.notes.begin(), "two-stage component rules: " + std::to_string(locked) + " decoupling capacitor(s) and IC(s) locked after stage 1, then refined");
  return x;
}

Placed place_with_fallbacks_one(const io::LoadedBoard& lb, const model::DesignRules& rules, const std::string& in, place::ExtractOptions eo,
                                const place::PlaceOptions& o, bool no_fallback) {
  Placed x;
  // Refine keeps the human's spacing rule (KiCad's default courtyard clearance is 0); full mode aims for
  // 0.25 mm and falls back to 0 when the board is too dense for it.
  if (o.mode == "refine") eo.default_clearance = 0;
  x.p = place::extract(lb.board, rules, in, eo);
  x.pl = place::Placement::initial(x.p);
  x.r = place::place(x.p, x.pl, o);
  std::vector<std::string> fallbacks;
  if (o.mode == "full" && !no_fallback && (!x.r.legal || x.r.legalise_failed > 0) && x.p.clearance_is_default && x.p.clearance > 0) {
    fallbacks.push_back("full mode with 0.25 mm courtyard clearance left " + std::to_string(x.r.legalise_failed) +
                        " parts unplaced: retried with KiCad's default courtyard clearance (0)");
    eo.default_clearance = 0;
    x.p = place::extract(lb.board, rules, in, eo);
    x.pl = place::Placement::initial(x.p);
    x.r = place::place(x.p, x.pl, o);
  }
  // Still failing: a part whose courtyard fits nowhere is placed by its pads instead (box + 0.25 mm), up to three
  // rounds. KiCad then reports a courtyard overlap for it, as it does for the designer's own placement of such
  // parts; the alternative below leaves the part where the input has it, which on a board without a placement
  // is on top of the others.
  for (int round = 0; round < 3 && o.mode == "full" && !no_fallback && x.r.legalise_failed > 0; ++round) {
    std::size_t added = 0;
    for (const auto& ref : x.r.legalise_failures)
      if (std::find(eo.pads_only.begin(), eo.pads_only.end(), ref) == eo.pads_only.end()) {
        eo.pads_only.push_back(ref);
        ++added;
      }
    if (added == 0) break;
    fallbacks.push_back("ignored the courtyard of " + std::to_string(added) + " part(s) that found no legal position (pad box + 0.25 mm instead) and placed again");
    x.p = place::extract(lb.board, rules, in, eo);
    x.pl = place::Placement::initial(x.p);
    x.r = place::place(x.p, x.pl, o);
  }
  // Still failing: keep the parts that found no place at their input position (as fixed parts) and place
  // the rest around them, up to six rounds.
  // Then by copper and holes only (scratch): the part may lie across other parts' courtyards, as battery holders,
  // modules and headers do in designers' own placements, but its pads and holes keep their clearances.
  for (int round = 0; round < 2 && eo.scratch && o.mode == "full" && !no_fallback && x.r.legalise_failed > 0; ++round) {
    std::size_t added = 0;
    for (const auto& ref : x.r.legalise_failures)
      if (std::find(eo.copper_only.begin(), eo.copper_only.end(), ref) == eo.copper_only.end()) {
        eo.copper_only.push_back(ref);
        ++added;
      }
    if (added == 0) break;
    fallbacks.push_back(std::to_string(added) + " part(s) still found no position: placed by their copper and holes only (they may lie across other courtyards)");
    x.p = place::extract(lb.board, rules, in, eo);
    x.pl = place::Placement::initial(x.p);
    x.r = place::place(x.p, x.pl, o);
  }
  // From scratch there is no input placement to fall back to: a part that still fits nowhere is set down beside
  // the board (to the right of it, in a row, as KiCad leaves new footprints), where it overlaps nothing, and
  // reported. The board is then not finished, but nothing is shorted and the rest can be routed.
  if (eo.scratch && o.mode == "full" && !no_fallback && x.r.legalise_failed > 0) {
    Coord cursor = x.p.region.x1 + 2'000'000;
    int parked = 0;
    for (const auto& ref : x.r.legalise_failures)
      for (std::size_t i = 0; i < x.p.parts.size(); ++i) {
        const auto& pt = x.p.parts[i];
        if (pt.ref != ref || !pt.movable) continue;
        const auto& body = pt.geom[0].body;  // offsets from the footprint origin
        x.pl.pos[i] = tmk::geom::Point{cursor - body.x0, x.p.region.y0 - body.y0};
        x.pl.rot[i] = 0;
        cursor += (body.x1 - body.x0) + 1'000'000;
        ++parked;
      }
    if (parked) fallbacks.push_back(std::to_string(parked) + " part(s) fit nowhere on the board and were set down beside it: the placement is not complete");
  }
  for (int round = 0; round < 6 && o.mode == "full" && !no_fallback && !eo.scratch && x.r.legalise_failed > 0; ++round) {
    int kept = 0;
    for (const auto& ref : x.r.legalise_failures)
      for (auto& pt : x.p.parts)
        if (pt.ref == ref && pt.movable) {
          pt.movable = false;
          pt.fixed_reason = "no legal position found in full mode: kept at its input position";
          ++kept;
        }
    if (kept == 0) break;
    fallbacks.push_back("kept " + std::to_string(kept) + " part(s) that found no legal position at their input positions and placed the rest again");
    x.pl = place::Placement::initial(x.p);
    x.r = place::place(x.p, x.pl, o);
  }
  if (o.mode == "full" && !no_fallback && !eo.scratch && (!x.r.legal || x.r.legalise_failed > 0)) {
    fallbacks.push_back("full mode could not place every part legally (" + std::to_string(x.r.legalise_failed) +
                        " unplaced): output is the refine-mode result instead");
    place::PlaceOptions ro = o;
    ro.mode = "refine";
    x.pl = place::Placement::initial(x.p);
    x.r = place::place(x.p, x.pl, ro);
  }
  for (auto it = fallbacks.rbegin(); it != fallbacks.rend(); ++it) x.r.notes.insert(x.r.notes.begin(), *it);
  return x;
}

// Applies a placement to the loaded document; returns the number of footprints moved.
int apply(io::LoadedBoard& lb, const place::Problem& p, const place::Placement& pl, std::uint64_t seed) {
  io::BoardEditor ed(lb, seed);
  int moved = 0;
  for (std::size_t i = 0; i < p.parts.size(); ++i) {
    const auto& pt = p.parts[i];
    if (pl.pos[i] == pt.pos0 && pl.rot[i] == 0) continue;
    if (place::flipped(pl.rot[i])) ed.flip_footprint(static_cast<std::size_t>(pt.fp), pl.pos[i], pt.angle_of(pl.rot[i]));
    else ed.move_footprint(static_cast<std::size_t>(pt.fp), pl.pos[i], pt.angle_of(pl.rot[i]));
    ++moved;
  }
  return moved;
}

// Side assignment (D48): which footprints the writer can mirror, and the user's side pins.
void setup_flip(const io::LoadedBoard& lb, bool flip, const std::string& keep_side, place::ExtractOptions& eo) {
  eo.flip = flip;
  if (!flip) return;
  eo.flip_ok.assign(lb.board.footprints.size(), 0);
  eo.flip_why.assign(lb.board.footprints.size(), {});
  for (std::size_t i = 0; i < lb.board.footprints.size(); ++i) eo.flip_ok[i] = io::flip_supported(lb, i, &eo.flip_why[i]) ? 1 : 0;
  std::string cur;
  for (char ch : keep_side + ",") {
    if (ch == ',' || ch == ' ') {
      if (!cur.empty()) eo.keep_side.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
}

// The router as a placement evaluator: the input document with the placement applied, routed in memory with a
// deterministic work budget; unrouted connections are mapped back to parts and pad positions.
place::RouteFn make_route_fn(const std::string& in, const model::DesignRules& rules, const place::Problem& p, long work, int threads,
                             std::uint64_t seed) {
  return [=, &rules, &p](const place::Placement& pl) {
    place::RouteEval e;
    io::LoadedBoard lb = io::read_board_file(in);
    apply(lb, p, pl, seed);
    const model::Board b = io::read_board(lb.doc);  // applies the pending edits (same board as the saved file)
    route::RouterOptions ro;
    ro.work_budget = work;
    ro.time_limit_s = 600;  // safety net only; the work budget decides
    ro.optimize = false;    // clean-up never changes the routed count, which is all the check reads
    const auto res = route::route_portfolio(b, rules, ro, threads).best;
    e.ok = true;
    e.connections = res.connections;
    e.routed = res.routed;
    e.seconds = res.seconds;
    std::map<std::string, int> part_of;
    for (std::size_t i = 0; i < p.parts.size(); ++i) part_of.emplace(p.parts[i].ref, static_cast<int>(i));
    // "REF.NUM" -> (part, pad position)
    auto locate = [&](const std::string& s, int& part, place::Point& at) {
      part = -1;
      for (std::size_t k = s.find('.'); k != std::string::npos; k = s.find('.', k + 1)) {
        const std::string ref = s.substr(0, k), num = s.substr(k + 1);
        const auto it = part_of.find(ref);
        if (it == part_of.end()) continue;
        part = it->second;
        for (int pi : b.footprints[static_cast<std::size_t>(p.parts[static_cast<std::size_t>(part)].fp)].pads)
          if (b.pads[static_cast<std::size_t>(pi)].number == num) {
            at = b.pads[static_cast<std::size_t>(pi)].pos;
            return;
          }
        at = pl.pos[static_cast<std::size_t>(part)];
        return;
      }
    };
    for (const auto& u : res.unrouted) {
      place::RouteEval::Failure f;
      f.net = u.net;
      locate(u.a, f.part_a, f.a);
      f.b = f.a;
      if (u.b != "zone") locate(u.b, f.part_b, f.b);
      e.failed.push_back(f);
    }
    return e;
  };
}


struct LoopCli {
  std::string in, out, json_path;
  place::PlaceOptions o;
  bool move_connectors = false, no_fallback = false, no_decap_affinity = false, scratch = false;
  std::string component_rules = "soft";  // --component-rules (doc 15; soft by default, D52)
  std::string rules_override;           // --rules-override (doc 15 §6.3)
  bool edge_attraction = false;         // --edge-attraction (doc 15 CONN-01)
  int decap_weight = place::kSignalWeight;
  int crules_weight_pct = 100;
  bool crules_two_stage = true;
  double clearance_mm = -1;
  long work = 3'000'000;
  int route_threads = 8;
  int rounds = 3, eco_rounds = 4, eco_candidates = 5;
  double beta = 1.0;
  double loop_time_s = 0;  // job wall-time stop for routable/eco (0 = none)
  long final_work = -1;    // routable/eco: verification budget for the winner vs the input (-1: 4 x work, 0: off)
  std::string record;      // routable: write the placement timelapse (JSONL) here
  std::string keep_side;   // --keep-side (D48)
};

// --component-rules (doc 15 §3.5, P1): detect component categories and add their proximity rules as objective-only
// pseudo-nets (crystal + load caps, oscillator, ESD at its connector, regulator caps, generalised decoupling). The
// D25 decoupling ties stay as they are. Returns the number of pseudo-nets requested (0 for off/report).
int apply_component_rules(const model::Board& b, const std::string& mode_s, const std::string& override_path, bool edge_attraction,
                          place::ExtractOptions& eo) {
  const crules::Mode mode = crules::parse_mode(mode_s);
  const auto& cat = crules::builtin_catalogue();
  crules::Overrides ov;
  if (!override_path.empty()) ov = crules::load_overrides_file(override_path, cat);  // validated even when off
  if (mode == crules::Mode::Off) {
    if (!override_path.empty()) std::printf("  warning: --rules-override %s has no effect with --component-rules off\n", override_path.c_str());
    return 0;
  }
  const auto det = crules::detect(b, cat, override_path.empty() ? nullptr : &ov);
  if (!override_path.empty()) {
    std::printf("  component rules: %zu user override(s) from %s\n", det.override_entries.size(), override_path.c_str());
    for (const auto& u : det.override_unused) std::printf("  warning: override matched nothing: %s\n", u.c_str());
  }
  std::map<std::string, int> per_cat;
  for (const auto& in : det.instances) ++per_cat[cat.categories[static_cast<std::size_t>(in.category)].id];
  std::string s;
  for (const auto& [k, n] : per_cat) s += " " + k + "=" + std::to_string(n);
  const auto aff = crules::placement_affinities(b, cat, det, mode);
  std::printf("  component rules (%s):%s; %zu proximity pseudo-net(s)\n", mode_s.c_str(), s.c_str(), aff.size());
  for (const auto& a : aff) eo.affinities.push_back({a.pad_a, a.pad_b, a.weight, a.name});
  if (edge_attraction) {
    const auto pulls = crules::edge_attractions(b, cat, det, mode);
    std::printf("  component rules: %zu connector edge pull candidate(s) (--edge-attraction)\n", pulls.size());
    for (const auto& e : pulls) eo.edge_pulls.push_back({e.footprint, e.body, e.weight, e.name});
  }
  return static_cast<int>(aff.size());
}

nlohmann::json eval_json(const place::RouteEval& e) {
  return {{"connections", e.connections}, {"routed", e.routed}, {"unrouted", e.unrouted()}, {"seconds", e.seconds}};
}

// --mode routable (doc 04 §3 G) and --mode eco (doc 04 §4): the router decides which placement is kept.
int run_loop_mode(const LoopCli& c) {
  auto lb = io::read_board_file(c.in);
  const auto rules = io::read_design_rules(c.in);
  place::ExtractOptions eo;
  eo.fix_edge_connectors = !c.move_connectors;
  eo.scratch = c.scratch;
  eo.decap_affinity = !c.no_decap_affinity;
  eo.decap_weight = c.decap_weight;
  eo.crules_weight_pct = c.crules_weight_pct;
  eo.crules_two_stage = c.crules_two_stage;
  apply_component_rules(lb.board, c.component_rules, c.rules_override, c.edge_attraction, eo);
  setup_flip(lb, c.o.flip, c.keep_side, eo);
  if (c.clearance_mm >= 0) eo.courtyard_clearance = mm_to_nm(c.clearance_mm);
  // The loop works on the refine problem (the board's courtyard rule, else KiCad's 0): every full-mode result,
  // placed with 0.25 mm or 0, is legal in it.
  place::ExtractOptions eo_refine = eo;
  eo_refine.default_clearance = 0;
  const place::Problem P = place::extract(lb.board, rules, c.in, eo_refine);
  const place::Placement input = place::Placement::initial(P);
  const place::Metrics input_m = place::measure(P, input);
  const bool input_legal = input_m.overlaps == 0 && input_m.outside == 0;
  const auto t0 = std::chrono::steady_clock::now();
  auto log = [](const std::string& s) {
    std::printf("  %s\n", s.c_str());
    std::fflush(stdout);
  };
  const place::RouteFn route = make_route_fn(c.in, rules, P, c.work, c.route_threads, c.o.seed);
  auto out_of_time = [&] {
    return c.loop_time_s > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > c.loop_time_s;
  };
  place::Candidate best;
  std::vector<place::Candidate> tried;
  PlaceRecorder rec;
  place::RouteEval input_eval;
  int routes = 0, rounds = 0;
  nlohmann::json extra = nlohmann::json::object();
  std::vector<std::string> notes;
  std::printf("%s: %zu parts (%d movable), mode %s, router budget %ld expansions x %d variants\n", c.in.c_str(), P.parts.size(), P.movable_count(),
              c.o.mode.c_str(), c.work, c.route_threads);
  // Compare against the input's minimal repair. If repair fails, keep the conservative model's conflicts visible.
  place::Placement base = input;
  std::string base_label = "input";
  bool base_legal = input_legal;
  if (!input_legal && !c.scratch) {  // from scratch the input is a pile, not a placement to repair
    std::string list;
    for (std::size_t i = 0; i < input_m.conflicts.size() && i < 6; ++i) list += (i ? ", " : "") + input_m.conflicts[i];
    if (input_m.conflicts.size() > 6) list += ", ...";
    std::string msg = "input not legal: " + std::to_string(input_m.conflicts.size()) +
                      " conflict(s) involving movable parts (" + list + ")";
    int n = 0;
    if (auto rep = place::repaired(P, input, &n)) {
      base = std::move(*rep);
      base_label = "input repaired (" + std::to_string(n) + " part(s) moved)";
      base_legal = true;
      msg += "; compared as its minimal repair: " + std::to_string(n) + " conflicting part(s) moved to the nearest legal spot";
    } else {
      msg += "; no minimal repair found: compared as it is";
    }
    log(msg);
    notes.push_back(msg);
  }
  if (c.o.mode == "eco") {
    input_eval = route(base);
    ++routes;
    log(base_label + ": " + std::to_string(input_eval.unrouted()) + " unrouted (" + std::to_string(input_eval.routed) + "/" +
        std::to_string(input_eval.connections) + ")");
    place::EcoOptions e;
    e.rounds = c.eco_rounds;
    e.candidates = c.eco_candidates;
    e.beta = c.beta;
    e.alpha_cross_mm = c.o.alpha_cross_mm;
    e.via_mm = c.o.flip ? c.o.via_mm : 0;
    e.log = log;
    const place::EcoResult er = place::eco_place(P, base, input_eval, e, route);
    routes += er.routes;
    // ECO checks each move, not existing conflicts.
    best = place::Candidate{er.committed > 0 ? "eco" : base_label, er.pl, er.eval, place::total_hpwl(P, er.pl), base_legal};
    extra["eco"] = {{"committed", er.committed}, {"ranked", er.ranked}, {"moves", er.moves}};
  } else {
    // From scratch (--scratch) the input is a pile, not a placement: it is no candidate, refine has nothing to
    // refine, legality is absolute instead of "no worse than the input", and there is no input to fall back to.
    std::vector<place::Candidate> seeds;
    std::vector<place::Candidate> illegal;  // scratch: the least bad choice when no seed is legal
    if (!c.scratch) seeds.push_back(place::Candidate{base_label, base, {}, 0});
    auto add_seed = [&](const std::string& label, const std::string& mode, double beta) {
      if (out_of_time()) {
        notes.push_back(label + ": not built, loop time limit reached");
        return;
      }
      // Plain seeds use the options as given (with the defaults they are exactly --mode auto's candidates); the
      // routability seeds add parallel tempering and LNS (doc 04 §8.1).
      place::PlaceOptions o = c.o;
      o.mode = mode;
      o.beta_congestion = beta;
      if (!c.record.empty()) {
        rec.phase("building candidate placement: " + label);
        o.trace = [&rec, label](const std::string& st, const place::Placement& x, double t) { rec.add(label + "|" + st, x, t); };
      }
      if (beta > 0) {
        o.tempering = true;
        o.lns_rate = std::max(o.lns_rate, 0.02);
      }
      Placed x = place_with_fallbacks(lb, rules, c.in, eo, o, c.no_fallback);
      bool same = x.p.parts.size() == P.parts.size();
      for (std::size_t i = 0; same && i < P.parts.size(); ++i) same = x.p.parts[i].fp == P.parts[i].fp;
      if (!same) {
        notes.push_back(label + ": part list differs from the loop problem, skipped");
        return;
      }
      const place::Metrics m = place::measure(P, x.pl, &input);
      // Legality is absolute in both modes; from scratch an unplaced part also disqualifies, and the candidate
      // is remembered as the least bad choice.
      if (!x.r.legal || (c.scratch && x.r.legalise_failed > 0) || m.overlaps > 0 || m.outside > 0) {
        notes.push_back(label + ": not legal, skipped" + (c.scratch ? " (" + std::to_string(m.overlaps) + " overlaps, " + std::to_string(m.outside) + " outside, " +
                                                                           std::to_string(x.r.legalise_failed) + " unplaced)" : std::string()));
        if (c.scratch) illegal.push_back(place::Candidate{label, x.pl, {}, 0});
        return;
      }
      seeds.push_back(place::Candidate{label, x.pl, {}, 0});
    };
    if (!c.scratch) {
      add_seed("refine", "refine", 0);
      add_seed("refine+rudy", "refine", c.beta);
    }
    add_seed("full", "full", 0);
    add_seed("full+rudy", "full", c.beta);
    bool illegal_fallback = false;
    if (seeds.empty() && !illegal.empty()) {
      notes.push_back("no legal placement was found: the output is the full-mode result with its conflicts");
      seeds.push_back(illegal.front());
      illegal_fallback = true;
    }
    place::LoopOptions lo;
    lo.place = c.o;
    lo.place.tempering = true;
    lo.beta = c.beta;
    lo.rounds = c.rounds;
    lo.eco_candidates = c.eco_candidates;
    lo.log = log;
    lo.out_of_time = out_of_time;
    // From scratch the only seed with conflicts is the least-bad fallback above, which must stay eligible.
    lo.accept_first_seed_conflicts = c.scratch ? illegal_fallback : !base_legal;
    if (!c.record.empty()) {
      lo.place.trace = [&rec](const std::string& key, const place::Placement& x, double t) {
        if (key.ends_with("|input")) rec.phase("re-placing around unrouted connections: " + key.substr(0, key.find('|')));
        rec.add(key, x, t);
      };
      lo.on_incumbent = [&rec](const std::string& label) { rec.chain.push_back(label); };
      lo.on_route = [&rec](const std::string& label) { rec.phase("router check: " + label); };
    }
    const place::LoopResult res = place::routability_loop(P, seeds, lo, route);
    best = res.best;
    if (best.pl.pos.empty())
      best = place::Candidate{base_label, base, {}, place::total_hpwl(P, base), base_legal};  // nothing routed
    tried = res.tried;
    routes = res.routes;
    rounds = res.rounds;
    if (c.scratch) {
      input_eval = route(input);
      ++routes;
    } else {
      input_eval = tried.front().eval;
    }
  }
  // Final verification (doc 04 §8.3b): at the check budget the router leaves connections unrouted that a full
  // route completes, so a placement that is easier for a short route can be harder for the real one. Route the
  // winner and the base (the input, or its minimal repair) again with a larger budget; the winner replaces the
  // base only if it is not worse there.
  nlohmann::json verify = nullptr;
  const bool best_is_base = best.pl.pos == base.pos && best.pl.rot == base.rot;
  if (const long fw = c.final_work < 0 ? 4 * c.work : c.final_work; fw > 0 && !c.scratch && !best_is_base) {
    const place::RouteFn route_full = make_route_fn(c.in, rules, P, fw, c.route_threads, c.o.seed);
    if (!c.record.empty()) rec.phase("verification: input and winner routed with a 4x budget");
    const place::RouteEval vi = route_full(base), vb = route_full(best.pl);
    routes += 2;
    const bool keep = vb.ok && vi.ok && vb.unrouted() <= vi.unrouted();
    log("verify at " + std::to_string(fw) + ": " + base_label + " " + std::to_string(vi.unrouted()) +
        " unrouted, " + best.label + " " + std::to_string(vb.unrouted()) +
        (keep ? " -> kept" : " -> " + base_label + " kept"));
    verify = {{"work", fw}, {"input", eval_json(vi)}, {"output", eval_json(vb)}, {"accepted", keep}};
    if (!keep) {
      rec.chain.clear();
      notes.push_back(best.label + " rejected by the final verification route (" + std::to_string(vb.unrouted()) + " vs " +
                      std::to_string(vi.unrouted()) + " unrouted)");
      best = place::Candidate{base_label, base, input_eval, place::total_hpwl(P, base), base_legal};
    }
  }
  const bool kept_input = best.pl.pos == input.pos && best.pl.rot == input.rot;
  if (!c.record.empty()) rec.write(c.record, P, input, best.pl, c.in);
  int moved = 0;
  if (kept_input) {
    std::filesystem::copy_file(c.in, c.out, std::filesystem::copy_options::overwrite_existing);
  } else {
    auto out_lb = io::read_board_file(c.in);
    moved = apply(out_lb, P, best.pl, c.o.seed);
    io::BoardEditor(out_lb, c.o.seed).save(c.out);
  }
  place::PlaceReport r;
  r.mode = c.o.mode;
  r.parts = static_cast<int>(P.parts.size());
  r.movable = P.movable_count();
  r.nets = static_cast<int>(P.nets.size());
  r.pins = static_cast<int>(P.pins.size());
  r.notes = notes;
  r.before = input_m;
  r.after = place::measure(P, best.pl, &input);
  r.legal = r.after.new_overlaps == 0 && r.after.new_outside == 0;
  r.lb_fixed_rot = place::hpwl_lower_bound(P, input, place::RotationModel::Fixed);
  r.lb_any_rot = place::hpwl_lower_bound(P, input, place::RotationModel::Any);
  r.seconds_total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  // Report unchanged input conflicts separately from illegal moves.
  const int left = r.after.overlaps + r.after.outside;
  const std::string status = !r.legal ? "NOT LEGAL"
                            : left > 0 ? "legal moves, " + std::to_string(left) + " conflict(s) of the input remain"
                                       : "legal";
  std::printf("  kept: %s, %d -> %d unrouted (%d/%d routed), HPWL %.1f -> %.1f mm, %d footprints moved, %d routes, %.1f s, %s -> %s\n",
              best.label.c_str(), input_eval.unrouted(), best.eval.unrouted(), best.eval.routed, best.eval.connections, nm_to_mm(r.before.hpwl),
              nm_to_mm(r.after.hpwl), moved, routes, r.seconds_total, status.c_str(), c.out.c_str());
  if (!c.json_path.empty()) {
    auto j = place::report_json(P, r);
    j["input"] = c.in;
    j["output"] = c.out;
    j["moved"] = moved;
    j["kept"] = best.label;
    j["routes"] = routes;
    j["rounds"] = rounds;
    j["route_input"] = eval_json(input_eval);
    j["route_output"] = eval_json(best.eval);
    // Same fields as --route-check, for the evaluation scripts.
    j["route_check"] = {{"work", c.work}, {"routed_input", input_eval.routed}, {"routed_output", best.eval.routed},
                        {"connections", best.eval.connections}, {"connections_input", input_eval.connections},
                        {"connections_output", best.eval.connections}, {"kept_input", kept_input}};
    nlohmann::json tj = nlohmann::json::array();
    for (const auto& t : tried) tj.push_back({{"label", t.label}, {"route", eval_json(t.eval)}, {"hpwl_mm", nm_to_mm(t.hpwl)}});
    j["tried"] = tj;
    j["verify"] = verify;
    for (auto& [k, v] : extra.items()) j[k] = v;
    std::ofstream(c.json_path) << j.dump(2) << "\n";
  }
  return r.legal ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace tmk;
  CLI::App app{"TraceMaker placer: quadratic + SimPL global placement, legalisation and annealing"};
  std::string in, out, json_path;
  place::PlaceOptions o;
  bool move_connectors = false, no_decap_affinity = false, scratch = false;
  std::string component_rules = "soft", rules_override;  // D52
  bool edge_attraction = false;
  int decap_weight = place::kSignalWeight;
  int crules_weight_pct = 100;
  bool crules_two_stage = true;
  double clearance_mm = -1;
  app.add_option("board", in, "Input .kicad_pcb")->required()->check(CLI::ExistingFile);
  app.add_option("-o,--output", out, "Output .kicad_pcb")->required();
  app.add_option("--mode", o.mode, "full (place from scratch) or refine (improve the current placement)")
      ->check(CLI::IsMember({"full", "refine", "auto", "routable", "eco"}));
  app.add_option("--time", o.time_limit_s, "Wall-time stop for the annealer, seconds (0 = none; budgets are in moves)");
  app.add_option("--seed", o.seed, "Random seed");
  app.add_option("--threads", o.threads, "Threads for parallel annealing runs (0 = min(cores, 16))");
  app.add_option("--runs", o.runs, "Independent annealing runs (0 = one per thread)");
  app.add_option("--effort", o.effort, "Annealing moves per run = effort x 4000 x movable parts");
  app.add_option("--alpha-cross-mm", o.alpha_cross_mm, "Cost of one airwire crossing in mm of signal HPWL");
  app.add_option("--courtyard-clearance-mm", clearance_mm, "Override the courtyard clearance (default: rules, else 0.25 mm)");
  app.add_flag("--move-connectors", move_connectors, "Also move connectors that touch the board edge");
  app.add_flag("--scratch", scratch, "The input has no placement (parts piled or beside the board, as after importing a schematic): "
                                     "input positions of movable parts carry no intent and are never fallen back to");
  app.add_flag("--no-decap-affinity", no_decap_affinity, "Do not tie decoupling capacitors to their IC's supply pins");
  app.add_option("--component-rules", component_rules,
                 "Component-aware layout rules (doc 15): off, report (detect and list only), soft (proximity pseudo-nets: crystal, ESD, regulator caps, generalised decoupling); default soft (D52)")
      ->check(CLI::IsMember({"off", "report", "soft", "on"}));
  app.add_option("--decap-weight", decap_weight, "Weight of each decoupling-capacitor tie (D25); a signal net weighs 10 (the default)")
      ->check(CLI::Range(1, 1000));
  app.add_option("--crules-weight", crules_weight_pct, "Scale of the component-rule proximity weights in percent (100 = as the rules say)")
      ->check(CLI::Range(1, 1000));
  app.add_flag("--crules-two-stage,!--no-crules-two-stage", crules_two_stage,
               "Full mode with component rules (default on): place with the decoupling ties first, lock those capacitors and their ICs, then refine with the component-rule pulls");
  app.add_flag("--edge-attraction", edge_attraction,
               "With --component-rules soft/on: pull movable connectors (CONN-01, USB2-11, DISP-02) toward the nearest board edge "
               "(objective only; pin headers are not pulled)");
  app.add_option("--rules-override", rules_override,
                 "User override file for --component-rules (JSON, doc 15 §6.3): disable rules, assert/deny categories, set parameters")
      ->check(CLI::ExistingFile);
  app.add_option("--json", json_path, "Write the placement report as JSON");
  app.add_flag("-v,--verbose", o.verbose, "Log the spreading iterations");
  std::string debug_part;
  bool no_fallback = false;
  long route_check = 0;  // routable/eco: the router work budget (default 3M)
  int route_threads = 8;
  app.add_option("--route-check", route_check,
                 "Route the input and the new placement with this work budget (A* expansions per variant) and keep the "
                 "placement that routes more connections (0 = off)");
  app.add_option("--route-threads", route_threads, "Router portfolio size for --route-check");
  app.add_flag("--no-fallback", no_fallback, "Full mode: keep the result even if some parts could not be placed");
  app.add_option("--debug-part", debug_part, "Print the legality map of one part and exit");
  LoopCli lc;
  app.add_flag("--tempering", o.tempering, "Parallel tempering over the annealing runs (replica exchange) instead of independent runs");
  app.add_option("--lns-rate", o.lns_rate, "Share of annealing steps that are large-neighbourhood (window rip-up and re-place) moves");
  app.add_option("--lns-window", o.lns_window, "Parts per LNS window (2..12)");
  app.add_option("--lns-polish", o.lns_polish, "LNS windows run on the annealing result");
  app.add_option("--beta", o.beta_congestion, "Routability weight: mm of signal HPWL per mm of RUDY overflow (0 = off)");
  std::string keep_side, debug_flip;
  app.add_flag("--flip", o.flip,
               "Side assignment (D48): movable surface-mount parts may move to the other side (KiCad-mirrored footprints); "
               "through-hole, locked and fixed parts never do");
  app.add_option("--flip-via-mm", o.via_mm, "With --flip: cost of one estimated via (a net's surface-mount pin on its minority side) in mm of signal HPWL");
  app.add_option("--flip-rate", o.flip_rate, "With --flip: share of annealing moves that are flips");
  app.add_option("--keep-side", keep_side, "With --flip: references that must stay on their side (comma separated)");
  app.add_option("--debug-flip", debug_flip,
                 "Write the input with these footprints (comma separated references, or 'all') flipped in place, KiCad style, and exit");
  app.add_option("--rounds", lc.rounds, "routable: re-placement rounds after the seeds");
  app.add_option("--eco-rounds", lc.eco_rounds, "eco: commit rounds");
  app.add_option("--eco-candidates", lc.eco_candidates, "eco/routable: moves routed per round");
  app.add_option("--loop-beta", lc.beta, "routable/eco: routability weight in the loop");
  app.add_option("--final-work", lc.final_work,
                 "routable/eco: route the winner and the input again with this budget; keep the winner only if not worse "
                 "(default 4 x --route-check, 0 = off)");
  app.add_option("--record", lc.record, "routable: write the placement timelapse (JSONL: header, then one frame per stage)");
  app.add_option("--loop-time", lc.loop_time_s,
                 "routable: wall-time stop in seconds; checked between seeds and routes, keeps the best placement so far (0 = none)");
  CLI11_PARSE(app, argc, argv);
  if (!debug_flip.empty()) {  // writer check: flip footprints in place (verified against pcbnew, scripts/flip_check.py)
    try {
      auto lb = io::read_board_file(in);
      io::BoardEditor ed(lb, o.seed);
      int n = 0;
      for (std::size_t i = 0; i < lb.board.footprints.size(); ++i) {
        const auto& fp = lb.board.footprints[i];
        if (debug_flip != "all" && ("," + debug_flip + ",").find("," + fp.reference + ",") == std::string::npos) continue;
        std::string why;
        if (!io::flip_supported(lb, i, &why)) {
          std::printf("  %s: not flipped (%s)\n", fp.reference.c_str(), why.c_str());
          continue;
        }
        ed.flip_footprint(i, fp.pos, -fp.angle);
        ++n;
      }
      ed.save(out);
      std::printf("flipped %d footprint(s) -> %s\n", n, out.c_str());
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: %s\n", e.what());
      return 1;
    }
  }
  if (edge_attraction && component_rules != "soft" && component_rules != "on") {
    std::fprintf(stderr, "error: --edge-attraction needs --component-rules soft or on\n");
    return 1;
  }

  if (o.mode == "routable" || o.mode == "eco") {
    lc.in = in;
    lc.out = out;
    lc.json_path = json_path;
    lc.o = o;
    lc.move_connectors = move_connectors;
    lc.scratch = scratch;
    lc.no_decap_affinity = no_decap_affinity;
    lc.component_rules = component_rules;
    lc.rules_override = rules_override;
    lc.edge_attraction = edge_attraction;
    lc.decap_weight = decap_weight;
    lc.crules_weight_pct = crules_weight_pct;
    lc.crules_two_stage = crules_two_stage;
    lc.clearance_mm = clearance_mm;
    lc.work = route_check > 0 ? route_check : 3'000'000;
    lc.route_threads = route_threads;
    lc.no_fallback = no_fallback;
    lc.keep_side = keep_side;
    try {
      return run_loop_mode(lc);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: %s\n", e.what());
      return 1;
    }
  }

  // auto: run refine and full (each with the route check against the input) and keep the more routable
  // result, then the shorter wirelength.
  if (o.mode == "auto") {
    if (route_check <= 0) {
      std::fprintf(stderr, "error: --mode auto needs --route-check\n");
      return 1;
    }
    struct Cand { std::string mode, path, json; int unrouted = 1 << 30; double hpwl = 0; int moved = 0; };
    std::vector<Cand> cands;
    for (const char* m : {"refine", "full"}) {
      Cand c{m, out + "." + m + ".kicad_pcb", out + "." + m + ".json"};
      std::string cmd = "'" + std::string(argv[0]) + "' '" + in + "' -o '" + c.path + "' --mode " + m + " --seed " + std::to_string(o.seed) +
                        " --threads " + std::to_string(o.threads) + " --effort " + std::to_string(o.effort) + " --route-check " +
                        std::to_string(route_check) + " --route-threads " + std::to_string(route_threads) + " --json '" + c.json + "'" +
                        (move_connectors ? " --move-connectors" : "") + (scratch ? " --scratch" : "") +
                        (no_decap_affinity ? " --no-decap-affinity" : "") + " --component-rules " + component_rules + " --decap-weight " + std::to_string(decap_weight) +
                        " --crules-weight " + std::to_string(crules_weight_pct) + (crules_two_stage ? "" : " --no-crules-two-stage") + (rules_override.empty() ? "" : " --rules-override '" + rules_override + "'") + (edge_attraction ? " --edge-attraction" : "") +
                        (o.flip ? " --flip --flip-via-mm " + std::to_string(o.via_mm) + " --flip-rate " + std::to_string(o.flip_rate) + (keep_side.empty() ? "" : " --keep-side '" + keep_side + "'") : "") +
                        " > /dev/null 2>&1";
      const int rc = std::system(cmd.c_str());
      if (rc != 0 && rc != 2 * 256) continue;
      try {
        std::ifstream f(c.json);
        const auto j = nlohmann::json::parse(f);
        const auto& rcj = j.at("route_check");
        const bool kept = rcj.at("kept_input").get<bool>();
        c.unrouted = kept ? rcj.at("connections_input").get<int>() - rcj.at("routed_input").get<int>()
                          : rcj.at("connections_output").get<int>() - rcj.at("routed_output").get<int>();
        c.moved = j.value("moved", 0);
        c.hpwl = j.at("after").value("hpwl_mm", 0.0);
        std::printf("  %s: %d unrouted, %d footprints moved%s\n", m, c.unrouted, c.moved,
                    rcj.at("kept_input").get<bool>() ? " (kept input)" : "");
        cands.push_back(c);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "  %s: no report (%s)\n", m, e.what());
      }
    }
    if (cands.empty()) {
      std::fprintf(stderr, "error: no candidate placement\n");
      return 1;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < cands.size(); ++i)
      if (cands[i].unrouted < cands[best].unrouted || (cands[i].unrouted == cands[best].unrouted && cands[i].moved > 0 && cands[i].hpwl < cands[best].hpwl)) best = i;
    std::filesystem::copy_file(cands[best].path, out, std::filesystem::copy_options::overwrite_existing);
    if (!json_path.empty()) std::filesystem::copy_file(cands[best].json, json_path, std::filesystem::copy_options::overwrite_existing);
    for (const auto& c : cands) {
      std::filesystem::remove(c.path);
      std::filesystem::remove(c.json);
    }
    std::printf("auto: kept %s -> %s\n", cands[best].mode.c_str(), out.c_str());
    return 0;
  }

  try {
    auto lb = io::read_board_file(in);
    const auto rules = io::read_design_rules(in);
    place::ExtractOptions eo;
    eo.fix_edge_connectors = !move_connectors;
    eo.scratch = scratch;
    eo.decap_affinity = !no_decap_affinity;
    eo.decap_weight = decap_weight;
    eo.crules_weight_pct = crules_weight_pct;
    eo.crules_two_stage = crules_two_stage;
    const int crules_ties = apply_component_rules(lb.board, component_rules, rules_override, edge_attraction, eo);
    setup_flip(lb, o.flip, keep_side, eo);
    if (clearance_mm >= 0) eo.courtyard_clearance = mm_to_nm(clearance_mm);
    // Refine keeps the human's spacing rule (KiCad's default courtyard clearance is 0); full mode aims for
    // 0.25 mm and falls back to 0 when the board is too dense for it.
    if (o.mode == "refine") eo.default_clearance = 0;
    place::Problem p = place::extract(lb.board, rules, in, eo);
    place::Placement pl = place::Placement::initial(p);
    if (!debug_part.empty()) {
      place::Legality L(p);
      L.reset(pl);
      for (std::size_t i = 0; i < p.parts.size(); ++i) {
        if (p.parts[i].ref != debug_part) continue;
        const auto& g = p.parts[i].geom[0];
        std::printf("%s movable=%d side=%d pos=(%.3f,%.3f) angle=%g body=[%.3f %.3f %.3f %.3f] edge_box=[%.3f %.3f %.3f %.3f] cy0=%zu cy1=%zu through=%zu pads=%zu\n",
                    debug_part.c_str(), p.parts[i].movable, p.parts[i].side, nm_to_mm(p.parts[i].pos0.x), nm_to_mm(p.parts[i].pos0.y),
                    p.parts[i].angle0, nm_to_mm(g.body.x0), nm_to_mm(g.body.y0), nm_to_mm(g.body.x1), nm_to_mm(g.body.y1),
                    nm_to_mm(g.edge_box.x0), nm_to_mm(g.edge_box.y0), nm_to_mm(g.edge_box.x1), nm_to_mm(g.edge_box.y1), g.cy[0].size(),
                    g.cy[1].size(), g.through.size(), g.pads.size());
        int inside = 0, legal = 0, total = 0;
        for (Coord y = p.region.y0; y <= p.region.y1; y += 250'000)
          for (Coord x = p.region.x0; x <= p.region.x1; x += 250'000) {
            ++total;
            if (!L.inside_ok(static_cast<int>(i), {x, y}, 0)) continue;
            ++inside;
            if (L.find_conflict(static_cast<int>(i), {x, y}, 0) < 0) ++legal;
          }
        std::printf("grid %d positions: %d inside, %d legal; at original: inside=%d conflict=%d\n", total, inside, legal,
                    L.inside_ok(static_cast<int>(i), p.parts[i].pos0, 0), L.find_conflict(static_cast<int>(i), p.parts[i].pos0, 0));
      }
      return 0;
    }
    Placed x = place_with_fallbacks(lb, rules, in, eo, o, no_fallback);
    p = std::move(x.p);
    pl = std::move(x.pl);
    place::PlaceReport r = std::move(x.r);
    int moved = apply(lb, p, pl, o.seed);
    io::BoardEditor ed(lb, o.seed);
    ed.save(out);

    // Router in the loop: a placement that routes fewer connections than the input is not an improvement,
    // whatever its wirelength. Route both with the same deterministic budget and keep the better one.
    int routed_in = -1, routed_out = -1, conns = 0, conns_in = 0, conns_out = 0;
    bool reverted = false;
    if (route_check > 0) {
      route::RouterOptions ro;
      ro.work_budget = route_check;
      ro.time_limit_s = 600;  // safety net only; the work budget decides
      auto routed_of = [&](const std::string& path) {
        const auto b = io::read_board_file(path).board;
        const auto rr = io::read_design_rules(path);
        const auto res = route::route_portfolio(b, rr, ro, route_threads).best;
        conns = res.connections;
        return res.routed;
      };
      routed_in = routed_of(in);
      conns_in = conns;
      routed_out = routed_of(out);
      conns_out = conns;
      if (scratch) {
        // From scratch the input is a pile: no alternative to fall back to and nothing to repair.
        r.notes.push_back("route check: new placement routed " + std::to_string(routed_out) + "/" + std::to_string(conns_out) + " (input " +
                          std::to_string(routed_in) + "/" + std::to_string(conns_in) + ")");
      } else {
        // Compare unrouted counts against the input's minimal repair, or the input if repair fails (D75).
        // Use the board's courtyard rule (else 0): full mode's 0.25 mm default can reject legal hand placements.
        place::ExtractOptions eo_input = eo;
        eo_input.default_clearance = 0;
        const place::Problem p_in = place::extract(io::read_board_file(in).board, rules, in, eo_input);
        const place::Placement in_pl = place::Placement::initial(p_in);
        const place::Metrics m_in = place::measure(p_in, in_pl);
        const bool input_legal = m_in.overlaps == 0 && m_in.outside == 0;
        int n = 0;
        const std::optional<place::Placement> rep = input_legal ? std::nullopt : place::repaired(p_in, in_pl, &n);
        const std::string head = "route check: new placement routed " + std::to_string(routed_out) + "/" +
                                 std::to_string(conns_out) + " (input " + std::to_string(routed_in) + "/" +
                                 std::to_string(conns_in) + ")" +
                                 (input_legal ? ""
                                              : "; input not legal (" + std::to_string(m_in.conflicts.size()) + " conflicts)");
        if (!rep) {
          if (conns_out - routed_out > conns_in - routed_in) {
            // The editor changed `lb` in place, so the input is restored by copying the file (byte for byte). Before
            // 2026-10-03 this re-saved the edited document, i.e. "kept the input" still wrote the new placement.
            std::filesystem::copy_file(in, out, std::filesystem::copy_options::overwrite_existing);
            reverted = true;
            moved = 0;
            pl = place::Placement::initial(p);
            r.after = place::measure(p, pl, &pl);
            r.notes.push_back(head + (input_legal ? "" : ", no minimal repair") + ": kept the input placement");
          } else {
            r.notes.push_back(head + (input_legal ? "" : ", no minimal repair"));
          }
        } else {
          const place::RouteEval re = make_route_fn(in, rules, p_in, route_check, route_threads, o.seed)(*rep);
          const std::string what = ", its minimal repair (" + std::to_string(n) + " part(s) moved) routed " +
                                   std::to_string(re.routed) + "/" + std::to_string(re.connections);
          if (conns_out - routed_out > re.unrouted()) {
            auto rlb = io::read_board_file(in);
            moved = apply(rlb, p_in, *rep, o.seed);
            io::BoardEditor(rlb, o.seed).save(out);
            pl = *rep;  // same part order: both problems come from the same board
            r.after = place::measure(p, pl, &in_pl);
            routed_out = re.routed;
            conns_out = re.connections;
            r.notes.push_back(head + what + ": kept the repair");
          } else {
            r.notes.push_back(head + what + ": new placement kept");
          }
        }
      }
    }

    auto mm = [](std::int64_t nm) { return static_cast<double>(nm) / 1e6; };
    const double w = place::kSignalWeight;
    std::printf("%s: %d parts (%d movable), %d nets, %d pins, mode %s\n", in.c_str(), r.parts, r.movable, r.nets, r.pins, r.mode.c_str());
    for (const auto& n : r.notes) std::printf("  note: %s\n", n.c_str());
    for (const auto& l : r.log) std::printf("  %s\n", l.c_str());
    std::printf("  HPWL            before %10.1f mm   after %10.1f mm\n", mm(r.before.hpwl), mm(r.after.hpwl));
    std::printf("  weighted HPWL   before %10.1f      after %10.1f   (signal x1, power x0.1)\n", mm(r.before.whpwl) / w, mm(r.after.whpwl) / w);
    std::printf("  lower bound L4  %10.1f (any rotation)  %10.1f (input rotations)  gap after/LB %.3f\n", mm(r.lb_any_rot) / w,
                mm(r.lb_fixed_rot) / w, r.lb_any_rot > 0 ? static_cast<double>(r.after.whpwl) / static_cast<double>(r.lb_any_rot) : 0.0);
    if (r.quadratic_whpwl >= 0)
      std::printf("  L1 quadratic    %10.1f (B2B optimum, overflow %.2f -> %.2f after %d SimPL iterations)\n", mm(r.quadratic_whpwl) / w,
                  r.overflow_quadratic, r.overflow_spread, r.spread_iterations);
    std::printf("  crossings       before %10lld      after %10lld\n", static_cast<long long>(r.before.crossings),
                static_cast<long long>(r.after.crossings));
    std::printf("  conflicts       before %10d      after %10d   new %d (outside board: %d -> %d, new %d; fixed-only pairs %d)\n",
                r.before.overlaps, r.after.overlaps, r.after.new_overlaps, r.before.outside, r.after.outside, r.after.new_outside,
                r.after.fixed_overlaps);
    if (o.flip)
      std::printf("  side assignment: %d part(s) may flip, %d flipped; via estimate before %lld after %lld\n", p.flippable_count(), r.after.flipped,
                  static_cast<long long>(r.before.side_vias), static_cast<long long>(r.after.side_vias));
    std::printf("  legalisation: %d placed, %d failed, mean displacement %.2f mm, max %.2f mm\n", r.legalise_placed, r.legalise_failed,
                r.legalise_mean_disp_mm, r.legalise_max_disp_mm);
    std::printf("  annealing: best run %d, %llu moves, %llu accepted%s\n", r.anneal.best_run, static_cast<unsigned long long>(r.anneal.moves),
                static_cast<unsigned long long>(r.anneal.accepted), r.anneal.time_limited ? " (stopped by --time)" : "");
    std::printf("  %d footprints moved, %.1f s, %s -> %s\n", moved, r.seconds_total, r.legal ? "legal" : "NOT LEGAL", out.c_str());
    if (!json_path.empty()) {
      auto j = place::report_json(p, r);
      j["input"] = in;
      j["output"] = out;
      j["moved"] = moved;
      j["component_rules"] = {{"mode", component_rules}, {"pseudo_nets", crules_ties}, {"edge_attraction", edge_attraction}, {"rules_override", rules_override}};
      if (route_check > 0) j["route_check"] = {{"work", route_check}, {"routed_input", routed_in}, {"routed_output", routed_out}, {"connections", conns}, {"connections_input", conns_in}, {"connections_output", conns_out}, {"kept_input", reverted}};
      // Final placement (footprint origins and courtyard boxes, mm) for plotting and inspection.
      nlohmann::json parts = nlohmann::json::array();
      for (std::size_t i = 0; i < p.parts.size(); ++i) {
        const auto& pt = p.parts[i];
        const auto& b = pt.geom[pl.rot[i]].body;
        parts.push_back({{"ref", pt.ref}, {"movable", pt.movable}, {"side", pt.side_in(pl.rot[i])}, {"flipped", place::flipped(pl.rot[i])},
                         {"x", nm_to_mm(pl.pos[i].x)}, {"y", nm_to_mm(pl.pos[i].y)}, {"x0", nm_to_mm(pt.pos0.x)}, {"y0", nm_to_mm(pt.pos0.y)},
                         {"angle", pt.angle_of(pl.rot[i])},
                         {"box", {nm_to_mm(b.x0 + pl.pos[i].x), nm_to_mm(b.y0 + pl.pos[i].y), nm_to_mm(b.x1 + pl.pos[i].x), nm_to_mm(b.y1 + pl.pos[i].y)}}});
      }
      j["placement"] = parts;
      nlohmann::json nets = nlohmann::json::array();
      for (const auto& n : p.nets) {
        if (n.affinity) continue;
        nlohmann::json pins = nlohmann::json::array();
        for (int pi : n.pins) {
          const auto q = pl.pin(p, pi);
          pins.push_back({nm_to_mm(q.x), nm_to_mm(q.y)});
        }
        nets.push_back({{"name", n.name}, {"signal", n.signal}, {"pins", pins}});
      }
      j["net_pins"] = nets;
      nlohmann::json outline = nlohmann::json::array();
      for (const auto& q : p.outline) outline.push_back({nm_to_mm(q.x), nm_to_mm(q.y)});
      j["outline"] = outline;
      std::ofstream(json_path) << j.dump(2) << "\n";
    }
    return r.legal ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
