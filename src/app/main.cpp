// tracemaker: command-line front end. Subcommands grow with the roadmap (route, place, bench, serve, replay).
#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "app/inspect.hpp"
#include "core/rng.hpp"
#include "drc/drc.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"
#include "server/messages.hpp"
#include "server/viewer_server.hpp"
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

struct ViewOptions {
  bool enabled = false;
  std::string host = "0.0.0.0";
  int port = 8766;
  bool hold = false;
};

int cmd_route(const std::string& in, const std::string& out, tmk::route::RouterOptions opt, const std::string& json_out, const ViewOptions& view) {
  auto lb = tmk::io::read_board_file(in);
  const auto rules = tmk::io::read_design_rules(in);
  std::unique_ptr<tmk::server::ViewerServer> server;
  if (view.enabled) {
    tmk::server::ServerOptions so;
    so.host = view.host;
    so.port = static_cast<std::uint16_t>(view.port);
    server = std::make_unique<tmk::server::ViewerServer>(so);
    server->publish(tmk::server::board_snapshot_json(lb.board, std::filesystem::path(in).filename().string()));
    std::printf("live view: %s\n", server->url().c_str());
    std::fflush(stdout);
    opt.sink = server.get();
  }
  tmk::route::Router router(lb.board, rules, opt);
  const auto res = router.run();
  tmk::io::BoardEditor ed(lb, opt.seed);
  for (const auto& t : res.tracks) ed.add_track(t);
  for (const auto& v : res.vias) ed.add_via(v);
  ed.save(out);
  std::printf("routed %d/%d connections, %zu tracks, %zu vias, pitch %.3f mm, %ld expansions, %.2f s\n", res.routed, res.connections,
              res.tracks.size(), res.vias.size(), tmk::nm_to_mm(res.pitch), res.expansions, res.seconds);
  for (const auto& f : res.failures) std::printf("  unrouted: %s\n", f.c_str());
  if (!json_out.empty()) {
    nlohmann::json j{{"routed", res.routed}, {"connections", res.connections}, {"tracks", res.tracks.size()}, {"vias", res.vias.size()},
                     {"seconds", res.seconds}, {"expansions", res.expansions}, {"pitch_mm", tmk::nm_to_mm(res.pitch)}, {"failures", res.failures}};
    std::ofstream(json_out) << j.dump(1);
  }
  if (server && view.hold) {
    std::printf("routing finished; viewer still serving at %s (Ctrl-C to quit)\n", server->url().c_str());
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return res.routed == res.connections ? 0 : 3;
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
  route->add_option("--time", ropt.time_limit_s, "Time limit, seconds");
  route->add_option("--pitch-um", r_pitch_um, "Lattice pitch in micrometres (default: automatic)");
  route->add_option("--via-cost-mm", ropt.via_cost_mm, "Cost of a via as equivalent track length");
  route->add_option("--seed", ropt.seed);
  route->add_option("--heuristic-weight", ropt.heuristic_weight, "Weighted A* factor (1.0 = optimal searches)");
  route->add_flag("!--no-rip-up", ropt.rip_up, "Disable negotiated rip-up and reroute");
  ViewOptions vopt;
  route->add_flag("--view", vopt.enabled, "Stream the routing live to the browser viewer");
  route->add_option("--view-host", vopt.host, "Viewer bind address (default 0.0.0.0)");
  route->add_option("--view-port", vopt.port, "Viewer port (default 8766)");
  route->add_flag("--hold", vopt.hold, "Keep serving the viewer after routing finishes");
  route->add_option("--json", r_json, "Write a result summary as JSON");

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
      if (!inspect_json.empty()) tmk::app::write_truth_json(lb.board, inspect_json);
      return 0;
    }
    if (*selftest) return cmd_selftest_edit(st_in, st_out);
    if (*drc) return cmd_drc(drc_path, drc_json, static_cast<tmk::Coord>(drc_eps_um * 1000.0));
    if (*pert) return cmd_perturb(pin, pout, pseed, ptracks, pvias, pmoves);
    if (*route) {
      ropt.pitch = static_cast<tmk::Coord>(r_pitch_um * 1000.0);
      return cmd_route(r_in, r_out, ropt, r_json, vopt);
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
