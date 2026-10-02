// tracemaker: command-line front end. Subcommands grow with the roadmap (route, place, bench, serve, replay).
#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

#include "app/inspect.hpp"
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
