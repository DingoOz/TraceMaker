// SPDX-License-Identifier: GPL-3.0-or-later
// tracemaker: command-line front end. Subcommands grow with the roadmap (route, place, bench, serve, replay).
#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

#include "app/defects.hpp"
#include "app/inspect.hpp"
#include "app/route_job.hpp"
#include "core/rng.hpp"
#include "crules/engine.hpp"
#include "drc/drc.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"
#include "learn/knowledge_base.hpp"
#include "route/diff_pair.hpp"
#include "route/escape.hpp"
#include "route/escape_flow.hpp"
#include "route/obstacles.hpp"
#include "core/version.hpp"
#include "gpu/device.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"

namespace {

int cmd_gpu_info() {
  if (!tmk::gpu::cuda_compiled()) {
    std::puts("This build has no CUDA support (cpu-only). CPU reference paths are used.");
    return 0;
  }
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) {
    std::puts("No CUDA devices visible. CPU reference paths will be used.");
    return 0;
  }
  constexpr double kMiB = 1024.0 * 1024.0;
  for (const auto& d : devices) {
    std::printf("cuda:%d  %-24s sm_%d%d  free %7.0f / %7.0f MiB  %s\n", d.cuda_index, d.name.c_str(), d.cc_major,
                d.cc_minor, static_cast<double>(d.free_bytes) / kMiB, static_cast<double>(d.total_bytes) / kMiB,
                d.uuid.c_str());
  }
  return 0;
}

// Applies a fixed set of edits (used by tests/integration to check edited files against KiCad):
// moves and rotates the first two unlocked footprints, removes the first track and via, adds a track and a via.
int cmd_selftest_edit(const std::string& in, const std::string& out) {
  auto lb = tmk::io::read_board_file(in);
  tmk::io::BoardEditor ed(lb, 7);
  auto& b = lb.board;
  int moved = 0;
  for (std::size_t i = 0; i < b.footprints.size() && moved < 2; ++i) {
    const auto& f = b.footprints[i];
    if (f.locked) continue;
    ed.move_footprint(i, {f.pos.x + 1'270'000, f.pos.y - 635'000}, f.angle + (moved == 0 ? 90.0 : 45.0));
    ++moved;
  }
  if (!b.tracks.empty()) ed.remove_track(0);
  if (!b.vias.empty()) ed.remove_via(0);
  const tmk::model::NetId net = b.nets.size() > 1 ? 1 : 0;
  ed.add_track({{10'000'000, 10'000'000}, {12'500'000, 12'500'000}, 250'000, 0, net, false, tmk::sexpr::kNoNode});
  ed.add_via({{12'500'000, 12'500'000}, 600'000, 300'000, 0, b.copper_count() - 1, tmk::model::ViaType::Through, net,
              false, tmk::sexpr::kNoNode});
  ed.save(out);
  std::printf("edited %s -> %s (%d footprints moved)\n", in.c_str(), out.c_str(), moved);
  return 0;
}

// Random perturbations that create DRC violations, for parity testing against KiCad's DRC:
// tracks between random pads, vias at random places, and small footprint moves.
int cmd_perturb(const std::string& in, const std::string& out, std::uint64_t seed, int tracks, int vias, int moves) {
  auto lb = tmk::io::read_board_file(in);
  tmk::io::BoardEditor ed(lb, seed);
  const auto& b = lb.board;
  const tmk::RngStream rng(seed, 0x9e27u, 0);
  std::uint64_t k = 0;
  auto u = [&](std::uint64_t n) { return n ? rng.u64(k++) % n : 0; };
  const auto bb = b.edge_bbox().empty() ? tmk::geom::Box{0, 0, 100'000'000, 100'000'000} : b.edge_bbox();
  auto rnd_pt = [&]() {
    return tmk::model::Point{bb.x0 + static_cast<tmk::Coord>(u(static_cast<std::uint64_t>(bb.x1 - bb.x0))),
                             bb.y0 + static_cast<tmk::Coord>(u(static_cast<std::uint64_t>(bb.y1 - bb.y0)))};
  };
  static const tmk::Coord widths[] = {80'000, 100'000, 150'000, 200'000, 250'000, 400'000};
  for (int i = 0; i < tracks && !b.pads.empty(); ++i) {
    const auto& p = b.pads[u(b.pads.size())];
    const tmk::model::Point a = p.pos;
    const tmk::model::Point c{a.x + static_cast<tmk::Coord>(u(6'000'000)) - 3'000'000, a.y + static_cast<tmk::Coord>(u(6'000'000)) - 3'000'000};
    int layer = 0;
    for (int l = 0; l < b.copper_count(); ++l)
      if (p.copper & tmk::model::layer_bit(l)) { layer = l; break; }
    ed.add_track({a, c, widths[u(6)], layer, p.net, false, tmk::sexpr::kNoNode});
  }
  static const tmk::Coord vsizes[][2] = {{600'000, 300'000}, {800'000, 400'000}, {450'000, 300'000}, {300'000, 200'000}};
  for (int i = 0; i < vias; ++i) {
    const auto& vs = vsizes[u(4)];
    const tmk::model::NetId net = static_cast<tmk::model::NetId>(u(b.nets.size()));
    ed.add_via({rnd_pt(), vs[0], vs[1], 0, b.copper_count() - 1, tmk::model::ViaType::Through, net, false, tmk::sexpr::kNoNode});
  }
  for (int i = 0; i < moves && !b.footprints.empty(); ++i) {
    const std::size_t fi = u(b.footprints.size());
    const auto& f = b.footprints[fi];
    if (f.locked) continue;
    ed.move_footprint(fi, {f.pos.x + static_cast<tmk::Coord>(u(3'000'000)) - 1'500'000, f.pos.y + static_cast<tmk::Coord>(u(3'000'000)) - 1'500'000}, f.angle);
  }
  ed.save(out);
  return 0;
}

int cmd_drc(const std::string& path, const std::string& json_out, tmk::Coord epsilon, bool linear_zones) {
  const auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::drc::DrcOptions opt;
  opt.epsilon = epsilon;
  opt.linear_zone_tests = linear_zones;
  const auto rep = tmk::drc::run_drc(lb.board, rules, opt);
  for (const auto& [type, n] : rep.counts()) std::printf("%-24s %d\n", type.c_str(), n);
  for (const auto& w : rep.warnings) std::printf("warning: %s\n", w.c_str());
  if (!json_out.empty()) tmk::drc::write_drc_json(rep, json_out);
  return rep.violations.empty() && rep.unconnected.empty() ? 0 : 5;
}

// Component-aware layout rules (design doc 15 §7): detections, bound roles and every rule's status. Read-only.
int cmd_rules(const std::string& path, const std::string& mode_s, const std::string& json_out, const std::string& dru_out, const std::string& roles_from,
              const std::string& catalogue_path, const std::string& override_path) {
  namespace cr = tmk::crules;
  const auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  const cr::Mode mode = cr::parse_mode(mode_s);
  cr::Catalogue own;
  if (!catalogue_path.empty()) own = cr::load_catalogue_file(catalogue_path);
  const cr::Catalogue& cat = catalogue_path.empty() ? cr::builtin_catalogue() : own;
  cr::Overrides ov;
  if (!override_path.empty()) ov = cr::load_overrides_file(override_path, cat);
  const cr::Overrides* ovp = override_path.empty() ? nullptr : &ov;
  cr::Detection det;
  if (!roles_from.empty()) {
    // Detect and bind on another version of the same board (e.g. the input of a placement), measure on this one:
    // binding that depends on positions (regulator caps) then names the same parts in both.
    const auto ref = tmk::io::read_board_file(roles_from);
    const auto& a = ref.board;
    bool same = a.footprints.size() == lb.board.footprints.size() && a.pads.size() == lb.board.pads.size() && a.nets.size() == lb.board.nets.size();
    for (std::size_t i = 0; same && i < a.footprints.size(); ++i) same = a.footprints[i].reference == lb.board.footprints[i].reference;
    for (std::size_t i = 0; same && i < a.nets.size(); ++i) same = a.nets[i].name == lb.board.nets[i].name;
    if (!same) throw std::runtime_error("--roles-from: " + roles_from + " is not the same board (footprints, pads or nets differ)");
    det = cr::detect(a, cat, ovp);
  } else {
    det = cr::detect(lb.board, cat, ovp);
  }
  const cr::Evaluation ev = cr::evaluate(lb.board, &rules, cat, det, mode);
  std::fputs(cr::report_text(lb.board, cat, det, ev).c_str(), stdout);
  if (!json_out.empty()) std::ofstream(json_out) << cr::report_json(lb.board, cat, det, ev).dump(1) << "\n";
  if (!dru_out.empty()) {
    std::ofstream(dru_out) << cr::dru_sidecar(lb.board, cat, det);
    std::printf("generated custom rules written to %s\n", dru_out.c_str());
  }
  return 0;
}

int cmd_route(tmk::app::RouteJob job) {
  job.log = [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  };
  return tmk::app::run_route_job(std::move(job)).exit_code();
}

// Prints the fixed-obstacle legality map around a pad (router debugging): '.' free for its net, '#' blocked,
// '+' the pad centre.
int cmd_debug_pad(const std::string& path, const std::string& ref, const std::string& num, double pitch_mm, double radius_mm, double width_mm,
                  bool via_map) {
  auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::model::Board b = lb.board;
  tmk::route::Obstacles obs(b, rules);
  for (const auto& p : b.pads) {
    const auto& fp = b.footprints[static_cast<std::size_t>(p.footprint)];
    if (fp.reference != ref || p.number != num) continue;
    const auto& nc = rules.class_for(b.nets[static_cast<std::size_t>(p.net)].name);
    const tmk::Coord hw = width_mm > 0 ? static_cast<tmk::Coord>(width_mm * 5e5) : std::max(nc.track_width, rules.minimums.track_width) / 2;
    const tmk::Coord pitch = static_cast<tmk::Coord>(pitch_mm * 1e6);
    const int n = static_cast<int>(radius_mm / pitch_mm);
    for (int l = 0; l < b.copper_count(); ++l) {
      if (!(p.copper & tmk::model::layer_bit(l))) continue;
      std::printf("%s.%s net %s layer %s hw %.3f mm; centre inside board: %d; outline points %zu\n", ref.c_str(), num.c_str(),
                  b.nets[static_cast<std::size_t>(p.net)].name.c_str(), b.copper_name(l).c_str(), tmk::nm_to_mm(hw), obs.inside_board(p.pos, 0),
                  obs.outline().size());
      for (int y = -n; y <= n; ++y) {
        std::string row;
        for (int x = -n; x <= n; ++x) {
          const tmk::geom::Point q{p.pos.x + x * pitch, p.pos.y + y * pitch};
          const auto code = obs.fixed_code(q, l, hw, 0, p.net);
          row += (x == 0 && y == 0) ? '+' : (code == tmk::route::Obstacles::kFree || code == p.net) ? '.' : '#';
        }
        std::printf("%s\n", row.c_str());
      }
    }
    if (via_map) {  // where a through via of the pad's net (net-class size) passes the fixed-copper checks
      const tmk::Coord drill = std::max(nc.via_drill, rules.minimums.through_hole_diameter);
      const tmk::Coord dia = std::max({nc.via_diameter, rules.minimums.via_diameter, drill + 2 * rules.minimums.via_annular_width});
      std::printf("via %.3f / %.3f mm (V legal)\n", tmk::nm_to_mm(dia), tmk::nm_to_mm(drill));
      for (int l = 0; l < b.copper_count(); ++l)
        std::printf("  centre layer %s: code %d\n", b.copper_name(l).c_str(), static_cast<int>(obs.fixed_code(p.pos, l, dia / 2, 0, p.net, true)));
      std::printf("  hole_to_hole %.3f, hole_clearance %.3f, via code %d\n", tmk::nm_to_mm(rules.minimums.hole_to_hole), tmk::nm_to_mm(rules.minimums.hole_clearance),
                  static_cast<int>(obs.fixed_via_code(p.pos, dia, drill, 0, p.net)));
      for (int y = -n; y <= n; ++y) {
        std::string row;
        for (int x = -n; x <= n; ++x) {
          const tmk::geom::Point q{p.pos.x + x * pitch, p.pos.y + y * pitch};
          const auto code = obs.fixed_via_code(q, dia, drill, 0, p.net);
          row += (x == 0 && y == 0) ? '+' : (code == tmk::route::Obstacles::kFree || code == p.net) ? 'V' : '#';
        }
        std::printf("%s\n", row.c_str());
      }
    }
    return 0;
  }
  std::fprintf(stderr, "pad not found\n");
  return 1;
}

// Differential pairs of a routed board (doc 05 §15): per pair the coupled share of its track length, the gap it
// keeps, the intra-pair skew and vias. Pairs: KiCad's by name, plus any given as "NET_A,NET_B".
int cmd_pairs(const std::string& path, const std::vector<std::string>& extra, const std::string& json_path) {
  auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::model::Board b = lb.board;
  tmk::route::Obstacles obs(b, rules);
  auto pairs = tmk::route::named_pairs(b, obs.rules());
  for (const auto& e : extra) {
    const auto comma = e.find(',');
    if (comma == std::string::npos) throw std::runtime_error("--pair wants NET_A,NET_B: " + e);
    auto id = [&](const std::string& name) {
      for (std::size_t i = 1; i < b.nets.size(); ++i)
        if (b.nets[i].name == name) return static_cast<tmk::model::NetId>(i);
      throw std::runtime_error("no net " + name);
    };
    pairs.emplace_back(id(e.substr(0, comma)), id(e.substr(comma + 1)));
  }
  nlohmann::json j = nlohmann::json::array();
  std::printf("%-28s %9s %9s %8s %9s %9s %8s %5s\n", "pair", "len_mm", "coupled", "gap_mm", "gap_min", "target", "skew_mm", "vias");
  for (const auto& [na, nb] : pairs) {
    const auto pr = tmk::route::pair_rule(b, rules, obs.rules(), na, nb);
    const auto st = tmk::route::measure_pair(b.tracks, b.vias, na, nb, tmk::route::coupled_threshold(pr));
    const std::string name = b.nets[static_cast<std::size_t>(na)].name + " / " + b.nets[static_cast<std::size_t>(nb)].name;
    std::printf("%-28s %9.2f %8.1f%% %8.3f %9.3f %9.3f %8.3f %2d/%-2d\n", name.c_str(), (st.length_a + st.length_b) / 2e6, 100 * st.coupled_share(),
                st.gap_median / 1e6, st.gap_min / 1e6, tmk::nm_to_mm(pr.gap), st.skew() / 1e6, st.vias_a, st.vias_b);
    j.push_back({{"net_a", b.nets[static_cast<std::size_t>(na)].name}, {"net_b", b.nets[static_cast<std::size_t>(nb)].name},
                 {"length_a_mm", st.length_a / 1e6}, {"length_b_mm", st.length_b / 1e6}, {"coupled_share", st.coupled_share()},
                 {"coupled_a_mm", st.coupled_a / 1e6}, {"coupled_b_mm", st.coupled_b / 1e6}, {"gap_median_mm", st.gap_median / 1e6},
                 {"gap_min_mm", st.gap_min / 1e6}, {"target_gap_mm", tmk::nm_to_mm(pr.gap)}, {"width_mm", tmk::nm_to_mm(pr.width)},
                 {"gap_source", pr.source}, {"skew_mm", st.skew() / 1e6}, {"vias_a", st.vias_a}, {"vias_b", st.vias_b}});
  }
  if (!json_path.empty()) std::ofstream(json_path) << nlohmann::json{{"board", path}, {"pairs", j}}.dump(1) << "\n";
  return 0;
}

int cmd_escape(const std::string& path, const std::string& json_path, bool flow) {
  auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::model::Board b = lb.board;
  tmk::route::Obstacles obs(b, rules);
  const auto parts = tmk::route::analyse_escapes(b, rules, obs);
  nlohmann::json j = nlohmann::json::array();
  int dead = 0, pins = 0;
  for (const auto& pe : parts) {
    pins += pe.pins;
    dead += static_cast<int>(pe.dead.size());
    std::printf("%-10s pitch %.3f mm: %d of %d pins escape%s%s\n", pe.ref.c_str(), tmk::nm_to_mm(pe.pitch), pe.escapable, pe.pins,
                pe.dead.empty() ? "" : "; dead:", pe.dead.empty() ? "" : "");
    std::map<std::string, std::vector<std::string>> by_reason;
    for (const auto& d : pe.dead) by_reason[d.reason].push_back(b.pads[static_cast<std::size_t>(d.pad)].number);
    nlohmann::json jd = nlohmann::json::array();
    for (const auto& [why, nums] : by_reason) {
      std::string list;
      for (std::size_t k = 0; k < nums.size() && k < 12; ++k) list += (k ? " " : "") + nums[k];
      if (nums.size() > 12) list += " ...";
      std::printf("    %zu pins, %s: %s\n", nums.size(), why.c_str(), list.c_str());
      jd.push_back({{"reason", why}, {"pins", nums}});
    }
    if (!pe.hint.empty()) std::printf("    hint: %s\n", pe.hint.c_str());
    j.push_back({{"ref", pe.ref}, {"pitch_mm", tmk::nm_to_mm(pe.pitch)}, {"pins", pe.pins}, {"escapable", pe.escapable}, {"dead", jd}, {"hint", pe.hint}});
  }
  std::printf("%zu dense packages, %d pins to route, %d cannot escape\n", parts.size(), pins, dead);
  nlohmann::json jf;
  if (flow) {
    // Escape plan v2 at the net classes' rules (the router's strict-pass width, clearance and class via).
    std::map<tmk::model::NetId, int> on_net;
    for (const auto& p : b.pads)
      if (p.net > 0) ++on_net[p.net];
    std::vector<char> needs(b.pads.size(), 0);
    for (std::size_t i = 0; i < b.pads.size(); ++i) needs[i] = b.pads[i].net > 0 && on_net[b.pads[i].net] > 1;
    auto nc = [&](tmk::model::NetId n) -> const tmk::model::NetClass& { return rules.class_for(b.nets[static_cast<std::size_t>(n)].name); };
    auto width = [&](tmk::model::NetId n) { return std::max(nc(n).track_width, rules.minimums.track_width); };
    auto via_d = [&](tmk::model::NetId n) {
      const tmk::Coord drill = std::max(nc(n).via_drill, rules.minimums.through_hole_diameter);
      return std::max({nc(n).via_diameter, rules.minimums.via_diameter, drill + 2 * rules.minimums.via_annular_width});
    };
    auto via_drill = [&](tmk::model::NetId n) { return std::max(nc(n).via_drill, rules.minimums.through_hole_diameter); };
    auto ok = [](std::int32_t c, tmk::model::NetId n) { return c == tmk::route::Obstacles::kFree || c == static_cast<std::int32_t>(n); };
    tmk::route::FlowEscapeInput in;
    in.width = width;
    in.clearance = [&](tmk::model::NetId n) { return std::max(nc(n).clearance, rules.minimums.clearance); };
    in.via = via_d;
    in.keep = [&](tmk::model::NetId n) { return width(n) + std::max(nc(n).clearance, rules.minimums.clearance); };
    in.track_free = [&](int l, tmk::geom::Point p, tmk::model::NetId n) { return ok(obs.fixed_code(p, l, width(n) / 2, 0, n), n); };
    in.via_free = [&](tmk::geom::Point p, tmk::model::NetId n) { return ok(obs.fixed_via_code(p, via_d(n), via_drill(n), 0, n), n); };
    in.layers = b.copper_count();
    tmk::route::FlowEscapeStats fs;
    tmk::route::plan_escapes_flow(b, needs, in, {}, &fs);
    std::printf("escape plan v2: %d deep arrays\n", fs.arrays);
    jf = {{"arrays", fs.arrays}, {"rings", nlohmann::json::array()}, {"per_layer", fs.per_layer}};
    for (std::size_t r = 0; r < fs.rings.size(); ++r) {
      const auto& rp = fs.rings[r];
      std::printf("  ring %2zu: %4d pins: %4d pad layer, %4d via + other layer, %4d via only, %4d none\n", r + 1, rp.pins, rp.pad_layer, rp.other_layer,
                  rp.via_only, rp.none);
      jf["rings"].push_back({{"ring", r + 1}, {"pins", rp.pins}, {"pad_layer", rp.pad_layer}, {"other_layer", rp.other_layer}, {"via_only", rp.via_only}, {"none", rp.none}});
    }
    std::printf("  escapes per layer:");
    for (std::size_t l = 0; l < fs.per_layer.size(); ++l) std::printf(" %s %d", b.copper_name(static_cast<int>(l)).c_str(), fs.per_layer[l]);
    std::printf("\n");
  }
  if (!json_path.empty()) {
    nlohmann::json out{{"board", path}, {"parts", j}, {"pins", pins}, {"dead", dead}};
    if (flow) out["flow"] = jf;
    std::ofstream(json_path) << out.dump(1) << "\n";
  }
  return 0;
}

int cmd_debug_seg(const std::string& path, const std::vector<double>& v, int layer, double width_mm, const std::string& netname) {
  auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::model::Board b = lb.board;
  tmk::route::Obstacles obs(b, rules);
  tmk::model::NetId net = 0;
  for (std::size_t i = 0; i < b.nets.size(); ++i)
    if (b.nets[i].name == netname) net = static_cast<tmk::model::NetId>(i);
  const tmk::geom::Point a{static_cast<tmk::Coord>(v[0] * 1e6), static_cast<tmk::Coord>(v[1] * 1e6)};
  const tmk::geom::Point e{static_cast<tmk::Coord>(v[2] * 1e6), static_cast<tmk::Coord>(v[3] * 1e6)};
  const tmk::Coord w = static_cast<tmk::Coord>(width_mm * 1e6);
  std::printf("segment_state %d (net %d)\n", obs.segment_state(a, e, layer, w, net, true, nullptr), static_cast<int>(net));
  const auto s = tmk::geom::Shape::segment(a, e, w / 2);
  for (const auto& it : obs.copper().items) {
    if (it.removed || !(it.layers & tmk::model::layer_bit(layer))) continue;
    for (const auto& sh : it.shapes) {
      const double g = tmk::geom::gap(s, sh);
      if (g < 1.0e6) std::printf("  item kind %d net %d gap %.4f mm\n", static_cast<int>(it.kind), static_cast<int>(it.net), g / 1e6);
    }
  }
  for (const auto& h : obs.copper().holes) {
    const double g = tmk::geom::gap(s, h.shape);
    if (g < 1.0e6) std::printf("  hole plated %d net %d clearance %.4f gap %.4f mm\n", h.plated ? 1 : 0, static_cast<int>(h.net), tmk::nm_to_mm(h.clearance), g / 1e6);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"TraceMaker: placement-aware PCB autorouter for KiCad"};
  app.require_subcommand(1);
  auto* version = app.add_subcommand("version", "Print the version");
  auto* gpu_info = app.add_subcommand("gpu-info", "List CUDA devices and their free memory");

  auto* inspect = app.add_subcommand("inspect", "Read a .kicad_pcb and print a summary");
  std::string inspect_path, inspect_json;
  inspect->add_option("board", inspect_path, "Board file")->required()->check(CLI::ExistingFile);
  inspect->add_option("--json", inspect_json, "Also write the board as JSON (KiCad truth schema)");

  auto* crules_cmd = app.add_subcommand("rules", "Detect component categories and report the component-aware layout rules (doc 15)");
  std::string cr_path, cr_json, cr_dru, cr_cat, cr_roles_from, cr_override, cr_mode = "soft";
  crules_cmd->add_option("board", cr_path, "Board file")->required()->check(CLI::ExistingFile);
  crules_cmd->add_option("--json", cr_json, "Write the full report (every rule of every instance) as JSON");
  crules_cmd->add_option("--mode", cr_mode, "Which statuses to report: report (check only), soft (as placement would apply them), on (with keep-outs)")
      ->check(CLI::IsMember({"report", "soft", "on"}));
  crules_cmd->add_option("--dru", cr_dru, "Write the generated custom rules (sidecar .kicad_dru) to this file");
  crules_cmd->add_option("--roles-from", cr_roles_from, "Detect and bind roles on this version of the board (e.g. the placement input), measure on the given one");
  crules_cmd->add_option("--catalogue", cr_cat, "Catalogue JSON to use instead of the built-in one (scripts/crules_catalogue.py output)");
  crules_cmd->add_option("--rules-override", cr_override,
                         "User override file (JSON, doc 15 §6.3): disable rules, assert/deny categories, set parameters")
      ->check(CLI::ExistingFile);

  auto* selftest = app.add_subcommand("selftest-edit", "Apply a fixed set of edits (for integration tests)");
  selftest->group("");  // hidden
  std::string st_in, st_out;
  selftest->add_option("in", st_in)->required();
  selftest->add_option("out", st_out)->required();

  auto* rt = app.add_subcommand("check-roundtrip", "Parse and re-write boards; report any that do not round-trip byte for byte");
  rt->group("");
  std::vector<std::string> rt_files;
  rt->add_option("boards", rt_files)->required();

  auto* drc = app.add_subcommand("drc", "Check a board against its KiCad design rules");
  std::string drc_path, drc_json;
  double drc_eps_um = 0.5;
  drc->add_option("board", drc_path)->required()->check(CLI::ExistingFile);
  drc->add_option("--json", drc_json, "Write the report as JSON (kicad-cli layout)");
  drc->add_option("--epsilon-um", drc_eps_um, "Tolerance below the required clearance, micrometres");
  bool drc_linear_zones = false;
  drc->add_flag("--linear-zones", drc_linear_zones, "Reference path: test items against every edge of a zone fill")->group("");

  auto* pert = app.add_subcommand("selftest-perturb", "Add random tracks/vias and footprint moves (DRC parity fuzzing)");
  pert->group("");
  std::string pin, pout;
  std::uint64_t pseed = 1;
  int ptracks = 30, pvias = 20, pmoves = 5;
  pert->add_option("in", pin)->required();
  pert->add_option("out", pout)->required();
  pert->add_option("--seed", pseed);
  pert->add_option("--tracks", ptracks);
  pert->add_option("--vias", pvias);
  pert->add_option("--moves", pmoves);

  auto* brk = app.add_subcommand("selftest-defects", "Inject one kind of defect into a routed board (broken-board DRC parity)");
  brk->group("");
  std::string bk_in, bk_out, bk_manifest, bk_kind;
  std::uint64_t bk_seed = 1;
  int bk_count = 3;
  brk->add_option("in", bk_in)->required()->check(CLI::ExistingFile);
  brk->add_option("out", bk_out)->required();
  brk->add_option("--kind", bk_kind)->required()->check(CLI::IsMember(tmk::app::defect_kinds()));
  brk->add_option("--seed", bk_seed);
  brk->add_option("--count", bk_count);
  brk->add_option("--manifest", bk_manifest, "Write the injected defects as JSON");

  auto* route = app.add_subcommand("route", "Route all unrouted connections of a board");
  std::string r_in, r_out, r_json;
  tmk::route::RouterOptions ropt;
  double r_pitch_um = 0;
  route->add_option("board", r_in)->required()->check(CLI::ExistingFile);
  route->add_option("-o,--output", r_out, "Output .kicad_pcb")->required();
  route->add_option("--time", ropt.time_limit_s, "Time limit, seconds (safety net; results then depend on machine speed)");
  route->add_option("--work", ropt.work_budget, "Deterministic work budget in search expansions per router (e.g. 50000000)");
  route->add_option("--pitch-um", r_pitch_um, "Lattice pitch in micrometres (default: automatic)");
  route->add_option("--via-cost-mm", ropt.via_cost_mm, "Cost of a via as equivalent track length");
  route->add_option("--seed", ropt.seed);
  route->add_option("--only-net", ropt.only_net, "Debugging: route only this net")->group("");
  route->add_option("--skip-net", ropt.skip_nets, "Experiments: leave this net unrouted (repeatable)")->group("");
  route->add_option("--soft-attempts", ropt.soft_attempts, "Window sizes tried by negotiated searches (1-4)")->group("");
  route->add_option("--heuristic-weight", ropt.heuristic_weight, "Weighted A* factor (1.0 = optimal searches)");
  route->add_flag("!--no-rip-up", ropt.rip_up, "Disable negotiated rip-up and reroute");
  route->add_flag("--micro-vias", ropt.micro_vias, "Use micro vias (outer layer to the next) where a through via is blocked (only on boards that allow them)");
  route->add_flag("--blind-vias", ropt.blind_vias, "Use blind/buried vias where a through via is blocked (only on boards that allow them)");
  route->add_flag("--diff-pairs", ropt.diff_pairs, "Route differential pairs (KiCad P/N or +/- names) as coupled pairs first (doc 05 §15)");
  double r_pair_skew_mm = 0;
  route->add_flag("--pair-twists,!--no-pair-twists", ropt.pair_twists, "Coupled pairs may swap sides at a layer change, for pairs whose pin order is mirrored between the ends (default on; only with pair routing)");
  route->add_option("--pair-skew-mm", r_pair_skew_mm, "Intra-pair skew limit for coupled pairs: meanders on the shorter half (0 = custom skew rules only)");
  route->add_flag("--global", ropt.global_route, "Global routing first: detailed search follows coarse corridors");
  route->add_flag("--global-confine", ropt.global_confine, "With --global: confine each connection's first search to its corridor (experimental)")->group("");
  route->add_flag("--global-corridor-only", ropt.global_strict_corridor_only, "With --global-confine: a corridor failure goes straight to negotiation (experimental)")->group("");
  route->add_option("--max-expansions", ropt.max_expansions, "Search expansions per attempt")->group("");
  route->add_flag("!--no-optimize", ropt.optimize, "Skip the post-routing clean-up pass (fewer vias, shorter tracks)");
  route->add_flag("--plan-gap,!--no-plan-gap", ropt.plan_gap, "Plan connections by copper-to-copper distance between pads rather than centre to centre (default on)");
  route->add_flag("--global-congestion", ropt.global_congestion, "Global plan as a congestion map: through traffic pays in tiles the plan fills (experimental)")->group("");
  route->add_option("--global-congestion-from", ropt.global_congestion_from, "Congestion threshold, eighths of tile capacity")->group("");
  route->add_option("--global-congestion-pen", ropt.global_congestion_pen, "Congestion cost, pitches per step per eighth above the threshold")->group("");
  route->add_flag("--escape-plan,!--no-escape-plan", ropt.escape_plan, "Reserve escape corridors for the pins of dense packages (QFP, BGA) before routing");
  route->add_flag("--escape-second-ring", ropt.escape_second_ring, "Escape plan: second-ring balls between two outer balls (else dog-bone vias)")->group("");
  route->add_flag("--escape-flow", ropt.escape_flow,
                  "Escape plan v2: min-cost-flow channel and layer assignment for deep BGA arrays (implies --escape-plan; experimental)");
  bool r_reroute = false;
  route->add_flag("--reroute", r_reroute, "Remove every unlocked track and via first and route the whole board again");
  route->add_flag("--fine-variant,!--no-fine-variant", ropt.fine_variant, "Portfolio: one variant routes small boards at half the lattice pitch (default on)")->group("");
  route->add_flag("--halving", ropt.halving, "With more --variants than --threads: successive halving over all the variants instead of running only the first ones");
  route->add_flag("--cut-report", ropt.cut_report, "Report straight lines across the board that more nets must cross than tracks fit (proof that the placement cannot be routed)");
  route->add_flag("--escape-report", ropt.escape_report, "Report the pins of deep BGA arrays per ring and how many were connected");
  route->add_flag("!--fast-bends", ropt.bend_states, "Approximate bend costs (1 state per lattice point instead of 9)");
  bool r_nogpu = false;
  route->add_flag("--no-gpu", r_nogpu, "Compute cost-to-go fields on the CPU instead of CUDA (same results)");
  route->add_flag("--reach-verify", ropt.reach_verify, "Test: check every unreachable verdict with the full A*")->group("");
  route->add_option("--reach-check", ropt.reach_check, "Reachability check before strict searches: 0 off, 1 likely failures, 2 all")->group("");
  route->add_flag("--soft-zones", ropt.soft_zones, "Zone fills do not block other nets (refill the zones afterwards); unused fills become plane targets");
  route->add_flag("--via-in-pad", ropt.via_in_pad, "Inner balls / enclosed SMD pads may take a minimum-size via in the pad (needs filled, capped vias)");
  route->add_flag("--keep-vias-off-pads", ropt.vias_off_pads, "Vias keep clear of SMD pads narrower than --vias-off-pads-below (via-in-pad excepted)");
  route->add_option("--vias-off-pads-below", ropt.vias_off_pads_below_mm, "Pad width (mm) below which --keep-vias-off-pads applies (default 2)");
  route->add_option("--first-nets", ropt.first_nets, "Comma-separated nets routed first and never ripped up by other nets (e.g. crystal lines)")->delimiter(',');
  route->add_flag("!--no-field", ropt.field_heuristic, "Use the octile heuristic only (no cost-to-go fields)");
  int r_threads = 8;
  std::string r_kb = tmk::learn::KnowledgeBase::default_path();
  bool r_nokb = false;
  route->add_option("--kb", r_kb, "Knowledge base file (failure memory across runs)");
  route->add_flag("--no-kb", r_nokb, "Do not read or update the knowledge base");
  route->add_option("--threads", r_threads,
                    "Threads for the portfolio (differently configured routers, best kept). Without --variants it is also the portfolio "
                    "size, except with --work, where all variants run and the output is identical for any thread count");
  int r_variants = 0;
  route->add_option("--variants", r_variants,
                    "Portfolio size (1 = single router; default: all with --work, else --threads); the output never depends on --threads "
                    "with --work. --time applies to each variant from its start");
  tmk::app::RouteJob r_job;
  route->add_flag("--view", r_job.view, "Stream the routing live to the browser viewer");
  route->add_option("--record", r_job.record, "Write the routing events (JSON lines, time-stamped) to a file for replay; zstd-compressed if FILE ends in .zst");
  route->add_option("--view-host", r_job.view_host, "Viewer bind address (default 0.0.0.0)");
  route->add_option("--view-port", r_job.view_port, "Viewer port (default 8766)");
  route->add_flag("--hold", r_job.hold, "Keep serving the viewer after routing finishes");
  route->add_option("--json", r_json, "Write a result summary as JSON");
  route->add_option("--component-rules", r_job.component_rules,
                    "Component-aware rules (doc 15): off (default); report/soft write <output>.tracemaker.kicad_dru; on also routes "
                    "with the generated keep-outs (crystal, switching-regulator inductor) as in-memory rule areas")
      ->check(CLI::IsMember({"off", "report", "soft", "on"}));
  route->add_option("--rules-override", r_job.rules_override,
                    "User override file for --component-rules (JSON, doc 15 §6.3): disable rules, assert/deny categories, set parameters")
      ->check(CLI::ExistingFile);
  std::string r_items;
  route->add_option("--emit-items", r_items, "Write the new tracks and vias as JSON (for the KiCad plugin)");

  auto* esc = app.add_subcommand("escape", "Escape feasibility of dense packages: pins that cannot leave their package under the board's rules");
  std::string esc_board, esc_json;
  esc->add_option("board", esc_board)->required()->check(CLI::ExistingFile);
  esc->add_option("--json", esc_json, "Write the analysis as JSON");
  bool esc_flow = false;
  esc->add_flag("--flow", esc_flow, "Also plan deep BGA arrays by min-cost flow and report the channel/layer assignment per ring");
  auto* pairs_cmd = app.add_subcommand("pairs", "Differential pairs of a routed board: coupled share, gap, intra-pair skew");
  std::string pr_board, pr_json;
  std::vector<std::string> pr_extra;
  pairs_cmd->add_option("board", pr_board)->required()->check(CLI::ExistingFile);
  pairs_cmd->add_option("--pair", pr_extra, "Also measure this pair (NET_A,NET_B), e.g. USB nets named DP/DM");
  pairs_cmd->add_option("--json", pr_json, "Write the measurements as JSON");
  auto* dseg = app.add_subcommand("debug-seg", "Explain the router's verdict on one segment");
  dseg->group("");
  std::string ds_board, ds_net;
  std::vector<double> ds_pts;
  int ds_layer = 0;
  double ds_width = 0.25;
  dseg->add_option("board", ds_board)->required();
  dseg->add_option("--pts", ds_pts)->expected(4)->required();
  dseg->add_option("--layer", ds_layer);
  dseg->add_option("--width", ds_width);
  dseg->add_option("--net", ds_net);
  auto* dbg = app.add_subcommand("debug-pad", "Print the router's legality map around a pad");
  dbg->group("");
  std::string d_board, d_ref, d_num;
  double d_pitch = 0.08, d_radius = 2.0, d_width = 0;
  bool d_via = false;
  dbg->add_option("board", d_board)->required();
  dbg->add_option("ref", d_ref)->required();
  dbg->add_option("pad", d_num)->required();
  dbg->add_option("--pitch", d_pitch);
  dbg->add_option("--radius", d_radius);
  dbg->add_option("--width", d_width);
  dbg->add_flag("--via", d_via, "Also print where a via of the pad's net is legal");

  CLI11_PARSE(app, argc, argv);
  try {
    if (*version) {
      std::printf("tracemaker %s\n", std::string(tmk::version()).c_str());
      return 0;
    }
    if (*gpu_info) return cmd_gpu_info();
    if (*inspect) {
      const auto lb = tmk::io::read_board_file(inspect_path);
      tmk::app::print_summary(lb.board, inspect_path);
      if (!inspect_json.empty()) {
        const auto rules = tmk::io::read_design_rules(inspect_path);
        tmk::app::write_truth_json(lb.board, inspect_json, &rules);
      }
      return 0;
    }
    if (*crules_cmd) return cmd_rules(cr_path, cr_mode, cr_json, cr_dru, cr_roles_from, cr_cat, cr_override);
    if (*selftest) return cmd_selftest_edit(st_in, st_out);
    if (*dseg) return cmd_debug_seg(ds_board, ds_pts, ds_layer, ds_width, ds_net);
    if (*esc) return cmd_escape(esc_board, esc_json, esc_flow);
    if (*pairs_cmd) return cmd_pairs(pr_board, pr_extra, pr_json);
    if (*dbg) return cmd_debug_pad(d_board, d_ref, d_num, d_pitch, d_radius, d_width, d_via);
    if (*drc) return cmd_drc(drc_path, drc_json, static_cast<tmk::Coord>(drc_eps_um * 1000.0), drc_linear_zones);
    if (*pert) return cmd_perturb(pin, pout, pseed, ptracks, pvias, pmoves);
    if (*brk) {
      const int n = tmk::app::inject_defects(bk_in, bk_out, bk_manifest, bk_kind, bk_seed, bk_count);
      std::printf("%d %s defects injected\n", n, bk_kind.c_str());
      return 0;
    }
    if (*route) {
      ropt.pitch = static_cast<tmk::Coord>(r_pitch_um * 1000.0);
      ropt.pair_skew = static_cast<tmk::Coord>(r_pair_skew_mm * 1e6);
      ropt.gpu_device = tmk::app::default_gpu_device(!r_nogpu);
      auto job = std::move(r_job);
      job.in = r_in;
      job.reroute = r_reroute;
      job.out = r_out;
      job.opt = ropt;
      job.threads = r_threads;
      job.variants = r_variants;
      job.kb_path = r_nokb ? std::string() : r_kb;
      job.items_out = r_items;
      job.json_out = r_json;
      return cmd_route(std::move(job));
    }
    if (*rt) {
      int bad = 0, ok = 0;
      for (const auto& f : rt_files) {
        try {
          const auto lb = tmk::io::read_board_file(f);
          if (lb.doc.write() != lb.doc.text()) throw std::runtime_error("write differs from input");
          if (!lb.board.warnings.empty()) std::printf("WARN %s: %s\n", f.c_str(), lb.board.warnings.front().c_str());
          ++ok;
        } catch (const std::exception& e) {
          ++bad;
          std::printf("FAIL %s: %s\n", f.c_str(), e.what());
        }
      }
      std::printf("%d ok, %d failed\n", ok, bad);
      return bad ? 1 : 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 1;
}
