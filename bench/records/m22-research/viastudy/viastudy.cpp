// Via / layer study (analysis only; not part of TraceMaker's source).
// For each open connection of a routed board: geometry of foreign copper near its end pads, and single-net re-routes on
// copies of the board (copper locked, as `tracemaker causes` does) with foreign vias or tracks removed near the ends, or
// with search settings relaxed (work, window, via cost, pitch). See the notes in research_notes/.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "drc/copper.hpp"
#include "geom/shape.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/escape.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

using namespace tmk;
using nlohmann::json;

namespace {

double mm(double nm) { return nm / 1e6; }

struct Ctx {
  model::Board board;
  model::DesignRules rules;
  std::vector<std::vector<geom::Shape>> pad_shapes;  // per pad
  std::set<std::pair<long long, long long>> input_vias;
};

double pad_gap(const Ctx& c, int pad, const geom::Shape& s) {
  double g = 1e18;
  for (const auto& ps : c.pad_shapes[static_cast<std::size_t>(pad)]) g = std::min(g, geom::gap(ps, s));
  return g;
}

route::RouteResult run(const Ctx& c, const model::Board& b, route::RouterOptions o, const std::string& only) {
  o.gpu_device = -1;
  o.time_limit_s = 3600;
  for (const auto& n : b.nets)
    if (n.id > 0 && !n.name.empty() && n.name != only) o.skip_nets.push_back(n.name);
  return route::Router(b, c.rules, o).run();
}

route::RouteResult run_multi(const Ctx& c, const model::Board& b, route::RouterOptions o, const std::set<std::string>& nets) {
  o.gpu_device = -1;
  o.time_limit_s = 3600;
  for (const auto& n : b.nets)
    if (n.id > 0 && !n.name.empty() && !nets.count(n.name)) o.skip_nets.push_back(n.name);
  return route::Router(b, c.rules, o).run();
}

std::set<int> open_pads(const route::RouteResult& r) {
  std::set<int> s;
  for (const auto& u : r.unrouted) {
    if (u.pad_a >= 0) s.insert(u.pad_a);
    if (u.pad_b >= 0) s.insert(u.pad_b);
  }
  return s;
}

struct Conn {
  std::string net;
  model::NetId nid = 0;
  int pad_a = -1, pad_b = -1;
  std::string a, b;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: viastudy ROUTED.kicad_pcb RULES_FROM.kicad_pcb OUT.json [threads] [max_nets] [exps]\n");
    return 2;
  }
  const std::string path = argv[1], rules_from = argv[2], out_path = argv[3];
  const int threads = argc > 4 ? std::atoi(argv[4]) : 8;
  const int max_nets = argc > 5 ? std::atoi(argv[5]) : 1000000;
  const std::string exps = argc > 6 ? argv[6] : "A,V05,V1,V2,T2,VT2,BIG,CHEAP,FINE,SWAP1";
  auto want = [&](const std::string& e) { return ("," + exps + ",").find("," + e + ",") != std::string::npos; };

  Ctx c;
  c.board = io::read_board_file(path).board;
  c.rules = io::read_design_rules(rules_from);
  for (auto& t : c.board.tracks) t.locked = true;
  for (auto& t : c.board.arcs) t.locked = true;
  for (auto& v : c.board.vias) v.locked = true;
  for (const auto& p : c.board.pads) c.pad_shapes.push_back(drc::pad_shapes(p));
  {
    const auto fx = io::read_board_file(rules_from).board;
    for (const auto& v : fx.vias) c.input_vias.insert({v.pos.x, v.pos.y});
  }
  const auto& B = c.board;

  // Open connections (plan only).
  route::RouterOptions po;
  po.plan_only = true;
  po.gpu_device = -1;
  const auto plan = route::Router(B, c.rules, po).run();
  std::vector<Conn> conns;
  for (const auto& u : plan.unrouted) conns.push_back({u.net, B.net_by_name(u.net), u.pad_a, u.pad_b, u.a, u.b});
  std::fprintf(stderr, "%s: %zu open connections, %d copper layers, pitch %.4f mm\n", path.c_str(), conns.size(), B.copper_count(), mm(static_cast<double>(plan.pitch)));

  // Escape analysis on the bare board: dense packages, dead pins.
  model::Board bare = B;
  bare.tracks.clear();
  bare.arcs.clear();
  bare.vias.clear();
  route::Obstacles obs(bare, c.rules);
  const auto parts = route::analyse_escapes(bare, c.rules, obs);
  std::map<int, double> dense_pitch;
  std::map<int, std::string> dead;
  for (const auto& pe : parts) {
    dense_pitch[pe.footprint] = mm(static_cast<double>(pe.pitch));
    for (const auto& d : pe.dead) dead[d.pad] = d.reason;
  }

  // Nets with a pad on a footprint.
  auto nets_of_fp = [&](int fp) {
    std::set<model::NetId> s;
    for (int q : B.footprints[static_cast<std::size_t>(fp)].pads) s.insert(B.pads[static_cast<std::size_t>(q)].net);
    return s;
  };

  // ---- Geometry around a pad: foreign vias and tracks.
  auto pad_env = [&](int pad) {
    const auto& p = B.pads[static_cast<std::size_t>(pad)];
    const auto own = nets_of_fp(p.footprint);
    json e;
    e["pad"] = B.footprints[static_cast<std::size_t>(p.footprint)].reference + "." + p.number;
    e["pos"] = {mm(static_cast<double>(p.pos.x)), mm(static_cast<double>(p.pos.y))};
    e["smd"] = p.type == model::PadType::Smd;
    e["part_pads"] = B.footprints[static_cast<std::size_t>(p.footprint)].pads.size();
    if (dense_pitch.count(p.footprint)) e["dense_pitch"] = dense_pitch[p.footprint];
    if (dead.count(pad)) e["dead"] = dead[pad];
    double nearest = 1e18, nearest_own = 1e18, nearest_thru = 1e18, nearest_input = 1e18;
    std::string nearest_net;
    int v[4] = {0, 0, 0, 0};          // foreign vias within 0.25, 0.5, 1, 2 mm (edge to edge)
    int v_thru[4] = {0, 0, 0, 0};     // ... of nets with no pad on this part
    double tl[4] = {0, 0, 0, 0};      // foreign track segments touching that distance (count)
    int tn[4] = {0, 0, 0, 0};
    const double R[4] = {0.25e6, 0.5e6, 1e6, 2e6};
    for (const auto& vi : B.vias) {
      if (vi.net == p.net) continue;
      if (std::llabs(vi.pos.x - p.pos.x) > 6'000'000 || std::llabs(vi.pos.y - p.pos.y) > 6'000'000) continue;
      const double g = pad_gap(c, pad, geom::Shape::point(vi.pos, vi.size / 2));
      const bool is_own = own.count(vi.net) > 0;
      if (g < nearest) nearest = g, nearest_net = B.nets[static_cast<std::size_t>(vi.net)].name;
      if (is_own) nearest_own = std::min(nearest_own, g);
      else nearest_thru = std::min(nearest_thru, g);
      if (c.input_vias.count({vi.pos.x, vi.pos.y})) nearest_input = std::min(nearest_input, g);
      for (int k = 0; k < 4; ++k)
        if (g < R[k]) ++v[k], v_thru[k] += is_own ? 0 : 1;
    }
    for (const auto& t : B.tracks) {
      if (t.net == p.net) continue;
      const Coord lo = 6'000'000;
      if (std::min(t.a.x, t.b.x) > p.pos.x + lo || std::max(t.a.x, t.b.x) < p.pos.x - lo || std::min(t.a.y, t.b.y) > p.pos.y + lo ||
          std::max(t.a.y, t.b.y) < p.pos.y - lo)
        continue;
      const double g = pad_gap(c, pad, geom::Shape::segment(t.a, t.b, t.width / 2));
      for (int k = 0; k < 4; ++k)
        if (g < R[k]) ++tn[k], tl[k] += 0;
    }
    e["via_gap_nearest_mm"] = nearest < 1e17 ? mm(nearest) : -1;
    e["via_nearest_net"] = nearest_net;
    e["via_gap_nearest_ownpart_mm"] = nearest_own < 1e17 ? mm(nearest_own) : -1;
    e["via_gap_nearest_through_mm"] = nearest_thru < 1e17 ? mm(nearest_thru) : -1;
    e["via_gap_nearest_input_mm"] = nearest_input < 1e17 ? mm(nearest_input) : -1;
    e["vias_within"] = {v[0], v[1], v[2], v[3]};
    e["vias_through_within"] = {v_thru[0], v_thru[1], v_thru[2], v_thru[3]};
    e["tracks_within"] = {tn[0], tn[1], tn[2], tn[3]};
    return e;
  };

  // Control group: every pad of the footprints that hold an open end (pads of other nets, with a net, not NPTH): open or not.
  std::set<int> open_end_pads;
  for (const auto& cn : conns) {
    open_end_pads.insert(cn.pad_a);
    if (cn.pad_b >= 0) open_end_pads.insert(cn.pad_b);
  }
  std::set<int> fps;
  for (int pd : open_end_pads) fps.insert(B.pads[static_cast<std::size_t>(pd)].footprint);
  json control = json::array();
  for (int fp : fps)
    for (int q : B.footprints[static_cast<std::size_t>(fp)].pads) {
      const auto& p = B.pads[static_cast<std::size_t>(q)];
      if (p.net == 0 || p.type == model::PadType::NpThruHole) continue;
      json e = pad_env(q);
      e["open"] = open_end_pads.count(q) > 0;
      control.push_back(std::move(e));
    }

  // ---- Copies.
  // Remove foreign vias / tracks whose gap to any of `pads` is below r.
  auto copy_without = [&](model::NetId keep_net, const std::vector<int>& pads, double r_vias, double r_tracks, std::set<model::NetId>* removed_via_nets) {
    model::Board cb = B;
    if (r_vias > 0)
      std::erase_if(cb.vias, [&](const model::Via& v) {
        if (v.net == keep_net) return false;
        for (int pd : pads)
          if (pad_gap(c, pd, geom::Shape::point(v.pos, v.size / 2)) < r_vias) {
            if (removed_via_nets) removed_via_nets->insert(v.net);
            return true;
          }
        return false;
      });
    if (r_tracks > 0) {
      std::erase_if(cb.tracks, [&](const model::Track& t) {
        if (t.net == keep_net) return false;
        for (int pd : pads)
          if (pad_gap(c, pd, geom::Shape::segment(t.a, t.b, t.width / 2)) < r_tracks) return true;
        return false;
      });
      std::erase_if(cb.arcs, [&](const model::ArcTrack& t) {
        if (t.net == keep_net) return false;
        for (int pd : pads)
          if (pad_gap(c, pd, geom::Shape::segment(t.a, t.mid, t.width / 2)) < r_tracks || pad_gap(c, pd, geom::Shape::segment(t.mid, t.b, t.width / 2)) < r_tracks)
            return true;
        return false;
      });
    }
    return cb;
  };

  // ---- Experiments per net.
  std::vector<std::string> nets;
  std::map<std::string, std::vector<std::size_t>> of_net;
  for (std::size_t i = 0; i < conns.size(); ++i) {
    if (!of_net.count(conns[i].net)) nets.push_back(conns[i].net);
    of_net[conns[i].net].push_back(i);
  }
  // Deterministic sample of nets when there are too many.
  std::vector<std::string> chosen;
  if (static_cast<int>(nets.size()) > max_nets) {
    const double step = static_cast<double>(nets.size()) / max_nets;
    for (int k = 0; k < max_nets; ++k) chosen.push_back(nets[static_cast<std::size_t>(k * step)]);
  } else chosen = nets;

  std::vector<json> res(conns.size());
  std::vector<bool> in_sample(conns.size(), false);
  std::mutex mu;
  auto conn_info = [&](const route::RouteResult& r, const Conn& cn) {
    json j;
    for (const auto& cr : r.conn_routes)
      if ((cr.pad_a == cn.pad_a && cr.pad_b == cn.pad_b) || (cr.pad_a == cn.pad_b && cr.pad_b == cn.pad_a)) {
        std::map<int, double> len;
        Coord wmin = INT64_MAX;
        for (const auto& t : cr.tracks) {
          len[t.layer] += mm(std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y)));
          wmin = std::min(wmin, t.width);
        }
        Coord vmin = INT64_MAX;
        for (const auto& v : cr.vias) vmin = std::min(vmin, v.size);
        const auto& nc = c.rules.class_for(cn.net);
        if (wmin != INT64_MAX) j["min_width_mm"] = mm(static_cast<double>(wmin));
        if (vmin != INT64_MAX) j["min_via_mm"] = mm(static_cast<double>(vmin));
        j["necked"] = (wmin != INT64_MAX && wmin < nc.track_width) || (vmin != INT64_MAX && vmin < nc.via_diameter);
        j["vias"] = cr.vias.size();
        json L = json::object();
        for (auto& [l, v] : len) L[std::to_string(l)] = v;
        j["len_by_layer"] = L;
        return j;
      }
    j["vias_net"] = r.vias.size();
    return j;
  };
  auto why_of = [&](const route::RouteResult& r, const Conn& cn) {
    for (const auto& u : r.unrouted)
      if ((u.pad_a == cn.pad_a && u.pad_b == cn.pad_b) || (u.pad_a == cn.pad_b && u.pad_b == cn.pad_a)) return u.reason + ": " + u.why;
    for (const auto& u : r.unrouted)
      if (u.pad_a == cn.pad_a || u.pad_b == cn.pad_b || u.pad_a == cn.pad_b || u.pad_b == cn.pad_a) return u.reason + ": " + u.why;
    return std::string();
  };

  auto do_net = [&](const std::string& name) {
    const auto& idx = of_net[name];
    const model::NetId nid = B.net_by_name(name);
    std::vector<std::size_t> todo;
    for (auto i : idx) {
      json j;
      const auto& cn = conns[i];
      j["net"] = cn.net;
      j["a"] = cn.a;
      j["b"] = cn.b;
      j["ends"] = json::array();
      bool any_dead = false;
      for (int pd : {cn.pad_a, cn.pad_b}) {
        if (pd < 0) continue;
        j["ends"].push_back(pad_env(pd));
        if (dead.count(pd)) any_dead = true;
      }
      j["dead_end"] = any_dead;
      j["exp"] = json::object();
      res[i] = std::move(j);
      todo.push_back(i);
    }
    std::vector<int> pads;
    for (auto i : idx) {
      pads.push_back(conns[i].pad_a);
      if (conns[i].pad_b >= 0) pads.push_back(conns[i].pad_b);
    }
    route::RouterOptions base;
    base.seed = 1;
    base.work_budget = 2'000'000;
    auto record = [&](const char* label, const route::RouteResult& r, const std::vector<std::size_t>& which) {
      const auto still = open_pads(r);
      std::vector<std::size_t> ok;
      for (auto i : which) {
        const auto& cn = conns[i];
        const bool routed = !(still.count(cn.pad_a) || (cn.pad_b >= 0 && still.count(cn.pad_b)));
        json e{{"routed", routed}, {"pitch", mm(static_cast<double>(r.pitch))}};
        if (routed) {
          e.update(conn_info(r, cn));
          ok.push_back(i);
        } else e["why"] = why_of(r, cn);
        res[i]["exp"][label] = e;
      }
      return ok;
    };
    // A: as is.
    const auto rA = run(c, B, base, name);
    record("A", rA, todo);
    // FB: the fast-bend search (one state per cell, parent direction; as the portfolio's variants 1-7). W: the variant that won on
    // most of these boards ("fast bends, cheap vias, 2x pitch on large boards, half pitch on small ones").
    if (want("FB")) {
      auto o = base;
      o.bend_states = false;
      record("FB", run(c, B, o, name), todo);
    }
    if (want("W")) {
      auto o = base;
      o.bend_states = false;
      o.via_cost_mm = base.via_cost_mm * 0.4;
      o.pitch_scale = 2.0;
      o.pitch_scale_small = 0.5;
      record("W", run(c, B, o, name), todo);
    }
    if (want("ONLYA")) return;
    std::vector<std::size_t> fail;
    for (auto i : todo)
      if (!res[i]["exp"]["A"]["routed"].get<bool>()) fail.push_back(i);
    if (fail.empty()) return;
    std::set<model::NetId> removed1;
    struct VE { const char* label; double rv, rt; };
    for (const VE& ve : {VE{"V05", 0.5e6, 0}, VE{"V1", 1e6, 0}, VE{"V2", 2e6, 0}, VE{"T2", 0, 2e6}, VE{"VT2", 2e6, 2e6}, VE{"VT1", 1e6, 1e6}}) {
      if (!want(ve.label)) continue;
      std::set<model::NetId> rem;
      const auto cb = copy_without(nid, pads, ve.rv, ve.rt, &rem);
      const auto r = run(c, cb, base, name);
      record(ve.label, r, fail);
      if (std::string(ve.label) == "V1") removed1 = rem;
      json rn = json::array();
      for (auto n : rem) rn.push_back(B.nets[static_cast<std::size_t>(n)].name);
      for (auto i : fail) res[i]["exp"][ve.label]["removed_via_nets"] = rn;
    }
    if (want("BIG")) {
      auto o = base;
      o.work_budget = 20'000'000;
      o.max_expansions = 20'000'000;
      o.soft_attempts = 4;
      record("BIG", run(c, B, o, name), fail);
    }
    if (want("CHEAP")) {
      auto o = base;
      o.work_budget = 4'000'000;
      o.via_cost_mm = 0.3;
      o.soft_attempts = 4;
      record("CHEAP", run(c, B, o, name), fail);
    }
    if (want("FINE")) {
      auto o = base;
      o.work_budget = 6'000'000;
      o.pitch_scale = 0.5;
      o.pitch_scale_small = 0.5;
      o.fine_variant = false;
      record("FINE", run(c, B, o, name), fail);
    }
    // SWAP1: the foreign vias within 1 mm were blockers; can this net and those nets all be routed if the router
    // places this net first? (Those nets' copper removed entirely, all routed again with this net first.)
    if (want("SWAP1") && want("V1")) {
      std::vector<std::size_t> v1ok;
      for (auto i : fail)
        if (res[i]["exp"].contains("V1") && res[i]["exp"]["V1"]["routed"].get<bool>()) v1ok.push_back(i);
      if (!v1ok.empty() && !removed1.empty()) {
        int pads_total = 0;
        for (auto n : removed1)
          for (const auto& p : B.pads)
            if (p.net == n) ++pads_total;
        json sw;
        sw["foreign_nets"] = removed1.size();
        sw["foreign_pads"] = pads_total;
        if (pads_total <= 60) {
          model::Board cb = B;
          std::erase_if(cb.vias, [&](const model::Via& v) { return removed1.count(v.net) > 0; });
          std::erase_if(cb.tracks, [&](const model::Track& t) { return removed1.count(t.net) > 0; });
          std::erase_if(cb.arcs, [&](const model::ArcTrack& t) { return removed1.count(t.net) > 0; });
          // Open connections of the foreign nets on the routed board (they may have had some already).
          std::set<std::string> fnames;
          for (auto n : removed1) fnames.insert(B.nets[static_cast<std::size_t>(n)].name);
          int foreign_open_before = 0;
          for (const auto& cn : conns)
            if (fnames.count(cn.net)) ++foreign_open_before;
          std::set<std::string> all = fnames;
          all.insert(name);
          auto o = base;
          o.work_budget = 6'000'000;
          o.first_nets = {name};
          const auto r = run_multi(c, cb, o, all);
          int foreign_open_after = 0;
          for (const auto& u : r.unrouted)
            if (fnames.count(u.net)) ++foreign_open_after;
          const auto still = open_pads(r);
          sw["foreign_open_before"] = foreign_open_before;
          sw["foreign_open_after"] = foreign_open_after;
          for (auto i : v1ok) {
            const auto& cn = conns[i];
            json e = sw;
            e["routed"] = !(still.count(cn.pad_a) || (cn.pad_b >= 0 && still.count(cn.pad_b)));
            e["all_ok"] = e["routed"].get<bool>() && foreign_open_after <= foreign_open_before;
            res[i]["exp"]["SWAP1"] = e;
          }
        } else
          for (auto i : v1ok) res[i]["exp"]["SWAP1"] = json{{"skipped", "foreign nets too large"}, {"foreign_pads", pads_total}};
      }
    }
  };

  for (const auto& n : chosen)
    for (auto i : of_net[n]) in_sample[i] = true;
  std::atomic<std::size_t> next{0};
  std::atomic<int> done{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < std::max(1, threads); ++t)
    pool.emplace_back([&] {
      for (std::size_t k; (k = next++) < chosen.size();) {
        do_net(chosen[k]);
        const int d = ++done;
        std::lock_guard<std::mutex> g(mu);
        std::fprintf(stderr, "  net %d/%zu %s\n", d, chosen.size(), chosen[k].c_str());
      }
    });
  for (auto& t : pool) t.join();

  json out;
  out["board"] = path;
  out["copper_layers"] = B.copper_count();
  out["pitch_mm"] = mm(static_cast<double>(plan.pitch));
  out["open"] = conns.size();
  out["nets_open"] = nets.size();
  out["nets_sampled"] = chosen.size();
  out["vias_on_board"] = B.vias.size();
  out["input_vias"] = c.input_vias.size();
  json cj = json::array();
  for (std::size_t i = 0; i < conns.size(); ++i)
    if (in_sample[i]) cj.push_back(res[i]);
  out["connections"] = cj;
  out["control"] = control;
  std::FILE* f = std::fopen(out_path.c_str(), "w");
  const auto s = out.dump(1);
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
  return 0;
}
