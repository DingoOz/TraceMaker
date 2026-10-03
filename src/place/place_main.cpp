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

struct Placed {
  place::Problem p;
  place::Placement pl;
  place::PlaceReport r;
};

// full or refine with the full-mode fallbacks (clearance 0, keeping unplaceable parts, refine as last resort).
Placed place_with_fallbacks(const io::LoadedBoard& lb, const model::DesignRules& rules, const std::string& in, place::ExtractOptions eo,
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
  // Still failing: keep the parts that found no place at their input position (as fixed parts) and place
  // the rest around them, up to six rounds.
  for (int round = 0; round < 6 && o.mode == "full" && !no_fallback && x.r.legalise_failed > 0; ++round) {
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
  if (o.mode == "full" && !no_fallback && (!x.r.legal || x.r.legalise_failed > 0)) {
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
    ed.move_footprint(static_cast<std::size_t>(pt.fp), pl.pos[i], pt.angle0 + 90.0 * pl.rot[i]);
    ++moved;
  }
  return moved;
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
  bool move_connectors = false, no_fallback = false;
  double clearance_mm = -1;
  long work = 3'000'000;
  int route_threads = 8;
  int rounds = 3, eco_rounds = 4, eco_candidates = 5;
  double beta = 1.0;
};

nlohmann::json eval_json(const place::RouteEval& e) {
  return {{"connections", e.connections}, {"routed", e.routed}, {"unrouted", e.unrouted()}, {"seconds", e.seconds}};
}

// --mode routable (doc 04 §3 G) and --mode eco (doc 04 §4): the router decides which placement is kept.
int run_loop_mode(const LoopCli& c) {
  auto lb = io::read_board_file(c.in);
  const auto rules = io::read_design_rules(c.in);
  place::ExtractOptions eo;
  eo.fix_edge_connectors = !c.move_connectors;
  if (c.clearance_mm >= 0) eo.courtyard_clearance = mm_to_nm(c.clearance_mm);
  // The loop works on the refine problem (the board's courtyard rule, else KiCad's 0): every full-mode result,
  // placed with 0.25 mm or 0, is legal in it.
  place::ExtractOptions eo_refine = eo;
  eo_refine.default_clearance = 0;
  const place::Problem P = place::extract(lb.board, rules, c.in, eo_refine);
  const place::Placement input = place::Placement::initial(P);
  const auto t0 = std::chrono::steady_clock::now();
  auto log = [](const std::string& s) {
    std::printf("  %s\n", s.c_str());
    std::fflush(stdout);
  };
  const place::RouteFn route = make_route_fn(c.in, rules, P, c.work, c.route_threads, c.o.seed);
  place::Candidate best;
  std::vector<place::Candidate> tried;
  place::RouteEval input_eval;
  int routes = 0, rounds = 0;
  nlohmann::json extra = nlohmann::json::object();
  std::vector<std::string> notes;
  std::printf("%s: %zu parts (%d movable), mode %s, router budget %ld expansions x %d variants\n", c.in.c_str(), P.parts.size(), P.movable_count(),
              c.o.mode.c_str(), c.work, c.route_threads);
  if (c.o.mode == "eco") {
    input_eval = route(input);
    ++routes;
    log("input: " + std::to_string(input_eval.unrouted()) + " unrouted (" + std::to_string(input_eval.routed) + "/" +
        std::to_string(input_eval.connections) + ")");
    place::EcoOptions e;
    e.rounds = c.eco_rounds;
    e.candidates = c.eco_candidates;
    e.beta = c.beta;
    e.alpha_cross_mm = c.o.alpha_cross_mm;
    e.log = log;
    const place::EcoResult er = place::eco_place(P, input, input_eval, e, route);
    routes += er.routes;
    best = place::Candidate{"eco", er.pl, er.eval, place::total_hpwl(P, er.pl)};
    extra["eco"] = {{"committed", er.committed}, {"ranked", er.ranked}, {"moves", er.moves}};
  } else {
    std::vector<place::Candidate> seeds;
    seeds.push_back(place::Candidate{"input", input, {}, 0});
    auto add_seed = [&](const std::string& label, const std::string& mode, double beta) {
      // Plain seeds use the options as given (with the defaults they are exactly --mode auto's candidates); the
      // routability seeds add parallel tempering and LNS (doc 04 §8.1).
      place::PlaceOptions o = c.o;
      o.mode = mode;
      o.beta_congestion = beta;
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
      if (!x.r.legal || m.new_overlaps > 0 || m.new_outside > 0) {
        notes.push_back(label + ": not legal, skipped");
        return;
      }
      seeds.push_back(place::Candidate{label, x.pl, {}, 0});
    };
    add_seed("refine", "refine", 0);
    add_seed("refine+rudy", "refine", c.beta);
    add_seed("full", "full", 0);
    add_seed("full+rudy", "full", c.beta);
    place::LoopOptions lo;
    lo.place = c.o;
    lo.place.tempering = true;
    lo.beta = c.beta;
    lo.rounds = c.rounds;
    lo.eco_candidates = c.eco_candidates;
    lo.log = log;
    const place::LoopResult res = place::routability_loop(P, seeds, lo, route);
    best = res.best;
    tried = res.tried;
    routes = res.routes;
    rounds = res.rounds;
    input_eval = tried.front().eval;
  }
  const bool kept_input = best.pl.pos == input.pos && best.pl.rot == input.rot;
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
  r.before = place::measure(P, input);
  r.after = place::measure(P, best.pl, &input);
  r.legal = r.after.new_overlaps == 0 && r.after.new_outside == 0;
  r.lb_fixed_rot = place::hpwl_lower_bound(P, input, place::RotationModel::Fixed);
  r.lb_any_rot = place::hpwl_lower_bound(P, input, place::RotationModel::Any);
  r.seconds_total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  kept: %s, %d -> %d unrouted (%d/%d routed), HPWL %.1f -> %.1f mm, %d footprints moved, %d routes, %.1f s, %s -> %s\n",
              best.label.c_str(), input_eval.unrouted(), best.eval.unrouted(), best.eval.routed, best.eval.connections, nm_to_mm(r.before.hpwl),
              nm_to_mm(r.after.hpwl), moved, routes, r.seconds_total, r.legal ? "legal" : "NOT LEGAL", c.out.c_str());
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
  bool move_connectors = false;
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
  app.add_option("--rounds", lc.rounds, "routable: re-placement rounds after the seeds");
  app.add_option("--eco-rounds", lc.eco_rounds, "eco: commit rounds");
  app.add_option("--eco-candidates", lc.eco_candidates, "eco/routable: moves routed per round");
  app.add_option("--loop-beta", lc.beta, "routable/eco: routability weight in the loop");
  CLI11_PARSE(app, argc, argv);

  if (o.mode == "routable" || o.mode == "eco") {
    lc.in = in;
    lc.out = out;
    lc.json_path = json_path;
    lc.o = o;
    lc.move_connectors = move_connectors;
    lc.clearance_mm = clearance_mm;
    lc.work = route_check > 0 ? route_check : 3'000'000;
    lc.route_threads = route_threads;
    lc.no_fallback = no_fallback;
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
                        (move_connectors ? " --move-connectors" : "") + " > /dev/null 2>&1";
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
      // Compare unrouted connections: the connection count can change with the placement.
      if (conns_out - routed_out > conns_in - routed_in) {
        // The editor changed `lb` in place, so the input is restored by copying the file (byte for byte). Before
        // 2026-10-03 this re-saved the edited document, i.e. "kept the input" still wrote the new placement.
        std::filesystem::copy_file(in, out, std::filesystem::copy_options::overwrite_existing);
        reverted = true;
        moved = 0;
        pl = place::Placement::initial(p);
        r.after = place::measure(p, pl, &pl);
        r.notes.push_back("route check: new placement routed " + std::to_string(routed_out) + "/" + std::to_string(conns) + " < input " +
                          std::to_string(routed_in) + ": kept the input placement");
      } else {
        r.notes.push_back("route check: new placement routed " + std::to_string(routed_out) + "/" + std::to_string(conns) + " (input " +
                          std::to_string(routed_in) + ")");
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
      if (route_check > 0) j["route_check"] = {{"work", route_check}, {"routed_input", routed_in}, {"routed_output", routed_out}, {"connections", conns}, {"connections_input", conns_in}, {"connections_output", conns_out}, {"kept_input", reverted}};
      // Final placement (footprint origins and courtyard boxes, mm) for plotting and inspection.
      nlohmann::json parts = nlohmann::json::array();
      for (std::size_t i = 0; i < p.parts.size(); ++i) {
        const auto& pt = p.parts[i];
        const auto& b = pt.geom[pl.rot[i]].body;
        parts.push_back({{"ref", pt.ref}, {"movable", pt.movable}, {"side", pt.side},
                         {"x", nm_to_mm(pl.pos[i].x)}, {"y", nm_to_mm(pl.pos[i].y)}, {"x0", nm_to_mm(pt.pos0.x)}, {"y0", nm_to_mm(pt.pos0.y)},
                         {"angle", pt.angle0 + 90.0 * pl.rot[i]},
                         {"box", {nm_to_mm(b.x0 + pl.pos[i].x), nm_to_mm(b.y0 + pl.pos[i].y), nm_to_mm(b.x1 + pl.pos[i].x), nm_to_mm(b.y1 + pl.pos[i].y)}}});
      }
      j["placement"] = parts;
      nlohmann::json nets = nlohmann::json::array();
      for (const auto& n : p.nets) {
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
