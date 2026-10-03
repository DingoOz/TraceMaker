// tracemaker: command-line front end. Subcommands grow with the roadmap (route, place, bench, serve, replay).
#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "app/inspect.hpp"
#include "app/route_job.hpp"
#include "core/rng.hpp"
#include "drc/drc.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"
#include "learn/knowledge_base.hpp"
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

int cmd_drc(const std::string& path, const std::string& json_out, tmk::Coord epsilon) {
  const auto lb = tmk::io::read_board_file(path);
  const auto rules = tmk::io::read_design_rules(path);
  tmk::drc::DrcOptions opt;
  opt.epsilon = epsilon;
  const auto rep = tmk::drc::run_drc(lb.board, rules, opt);
  for (const auto& [type, n] : rep.counts()) std::printf("%-24s %d\n", type.c_str(), n);
  for (const auto& w : rep.warnings) std::printf("warning: %s\n", w.c_str());
  if (!json_out.empty()) tmk::drc::write_drc_json(rep, json_out);
  return rep.violations.empty() && rep.unconnected.empty() ? 0 : 5;
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
int cmd_debug_pad(const std::string& path, const std::string& ref, const std::string& num, double pitch_mm, double radius_mm, double width_mm) {
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
    return 0;
  }
  std::fprintf(stderr, "pad not found\n");
  return 1;
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
  route->add_option("--soft-attempts", ropt.soft_attempts, "Window sizes tried by negotiated searches (1-4)")->group("");
  route->add_option("--heuristic-weight", ropt.heuristic_weight, "Weighted A* factor (1.0 = optimal searches)");
  route->add_flag("!--no-rip-up", ropt.rip_up, "Disable negotiated rip-up and reroute");
  route->add_flag("--global", ropt.global_route, "Global routing first: detailed search follows coarse corridors");
  route->add_flag("!--no-optimize", ropt.optimize, "Skip the post-routing clean-up pass (fewer vias, shorter tracks)");
  route->add_flag("!--fast-bends", ropt.bend_states, "Approximate bend costs (1 state per lattice point instead of 9)");
  bool r_nogpu = false;
  route->add_flag("--no-gpu", r_nogpu, "Compute cost-to-go fields on the CPU instead of CUDA (same results)");
  route->add_flag("!--no-field", ropt.field_heuristic, "Use the octile heuristic only (no cost-to-go fields)");
  int r_threads = 8;
  std::string r_kb = tmk::learn::KnowledgeBase::default_path();
  bool r_nokb = false;
  route->add_option("--kb", r_kb, "Knowledge base file (failure memory across runs)");
  route->add_flag("--no-kb", r_nokb, "Do not read or update the knowledge base");
  route->add_option("--threads", r_threads, "Portfolio size: differently configured routers run in parallel, best kept (1 = single router)");
  tmk::app::RouteJob r_job;
  route->add_flag("--view", r_job.view, "Stream the routing live to the browser viewer");
  route->add_option("--record", r_job.record, "Write the routing events (JSON lines, time-stamped) to a file for replay");
  route->add_option("--view-host", r_job.view_host, "Viewer bind address (default 0.0.0.0)");
  route->add_option("--view-port", r_job.view_port, "Viewer port (default 8766)");
  route->add_flag("--hold", r_job.hold, "Keep serving the viewer after routing finishes");
  route->add_option("--json", r_json, "Write a result summary as JSON");
  std::string r_items;
  route->add_option("--emit-items", r_items, "Write the new tracks and vias as JSON (for the KiCad plugin)");

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
  dbg->add_option("board", d_board)->required();
  dbg->add_option("ref", d_ref)->required();
  dbg->add_option("pad", d_num)->required();
  dbg->add_option("--pitch", d_pitch);
  dbg->add_option("--radius", d_radius);
  dbg->add_option("--width", d_width);

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
    if (*selftest) return cmd_selftest_edit(st_in, st_out);
    if (*dseg) return cmd_debug_seg(ds_board, ds_pts, ds_layer, ds_width, ds_net);
    if (*dbg) return cmd_debug_pad(d_board, d_ref, d_num, d_pitch, d_radius, d_width);
    if (*drc) return cmd_drc(drc_path, drc_json, static_cast<tmk::Coord>(drc_eps_um * 1000.0));
    if (*pert) return cmd_perturb(pin, pout, pseed, ptracks, pvias, pmoves);
    if (*route) {
      ropt.pitch = static_cast<tmk::Coord>(r_pitch_um * 1000.0);
      ropt.gpu_device = tmk::app::default_gpu_device(!r_nogpu);
      auto job = std::move(r_job);
      job.in = r_in;
      job.out = r_out;
      job.opt = ropt;
      job.threads = r_threads;
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
