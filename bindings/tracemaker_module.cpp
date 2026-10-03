// Python module `tracemaker` (roadmap M11, design doc 08 §5): the engine without a subprocess, for the KiCad
// plugin and for scripting. Routing goes through tmk::app::run_route_job, the same function the CLI's `route`
// subcommand calls, so `tracemaker.route(...)` and `tracemaker route ...` give identical boards.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/route_job.hpp"
#include "core/version.hpp"
#include "drc/drc.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "learn/knowledge_base.hpp"

namespace py = pybind11;

namespace {

struct BoardSummary {
  std::string path;
  long long version = 0;
  std::vector<std::string> copper_layers;
  std::vector<std::string> net_names;  // named nets, file order
  std::size_t footprints = 0, pads = 0, tracks = 0, arcs = 0, vias = 0, zones = 0;
  std::optional<std::pair<double, double>> outline_mm;  // edge-cut bounding box (width, height); None without Edge.Cuts
  std::vector<std::string> warnings;
  std::size_t nets() const { return net_names.size(); }
};

BoardSummary read_board(const std::string& path) {
  BoardSummary s;
  {
    py::gil_scoped_release nogil;
    const auto lb = tmk::io::read_board_file(path);
    const auto& b = lb.board;
    s.path = path;
    s.version = static_cast<long long>(b.version);
    for (int i = 0; i < b.copper_count(); ++i) s.copper_layers.push_back(b.copper_name(i));
    for (const auto& n : b.nets)
      if (!n.name.empty()) s.net_names.push_back(n.name);
    s.footprints = b.footprints.size();
    s.pads = b.pads.size();
    s.tracks = b.tracks.size();
    s.arcs = b.arcs.size();
    s.vias = b.vias.size();
    s.zones = b.zones.size();
    const auto bb = b.edge_bbox();
    if (!bb.empty()) s.outline_mm = std::make_pair(tmk::nm_to_mm(bb.x1 - bb.x0), tmk::nm_to_mm(bb.y1 - bb.y0));
    s.warnings = b.warnings;
  }
  return s;
}

py::object to_python(const nlohmann::json& j) {
  switch (j.type()) {
    case nlohmann::json::value_t::null: return py::none();
    case nlohmann::json::value_t::boolean: return py::bool_(j.get<bool>());
    case nlohmann::json::value_t::number_integer: return py::int_(j.get<std::int64_t>());
    case nlohmann::json::value_t::number_unsigned: return py::int_(j.get<std::uint64_t>());
    case nlohmann::json::value_t::number_float: return py::float_(j.get<double>());
    case nlohmann::json::value_t::string: return py::str(j.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
      py::list l;
      for (const auto& e : j) l.append(to_python(e));
      return l;
    }
    case nlohmann::json::value_t::object: {
      py::dict d;
      for (const auto& [k, v] : j.items()) d[py::str(k)] = to_python(v);
      return d;
    }
    default: return py::none();
  }
}

std::string opt_str(const std::optional<std::string>& s) { return s.value_or(std::string()); }

// Runs one routing job with the CLI's defaults. `kb`: True = default knowledge base, False/None = none, str = file.
tmk::app::RouteJobResult run_job(const std::string& in, const std::optional<std::string>& out, double time_s, int threads, long work,
                                 std::uint64_t seed, bool view, const std::string& view_host, int view_port, bool gpu, const py::object& kb,
                                 double pitch_um, double via_cost_mm, bool optimize, bool rip_up, bool global_route,
                                 const std::optional<std::string>& items_out, const std::optional<std::string>& json_out,
                                 const std::optional<std::string>& record, bool verbose, std::vector<std::string>& log) {
  tmk::app::RouteJob job;
  job.in = in;
  job.out = opt_str(out);
  job.opt.time_limit_s = time_s;
  job.opt.work_budget = work;
  job.opt.seed = seed;
  job.opt.pitch = static_cast<tmk::Coord>(pitch_um * 1000.0);
  job.opt.via_cost_mm = via_cost_mm;
  job.opt.optimize = optimize;
  job.opt.rip_up = rip_up;
  job.opt.global_route = global_route;
  job.threads = threads;
  if (py::isinstance<py::str>(kb)) job.kb_path = kb.cast<std::string>();
  else if (!kb.is_none() && kb.cast<bool>()) job.kb_path = tmk::learn::KnowledgeBase::default_path();
  job.items_out = opt_str(items_out);
  job.json_out = opt_str(json_out);
  job.view = view;
  job.view_host = view_host;
  job.view_port = view_port;
  job.record = opt_str(record);
  // Called on this thread only, without the GIL: collect, and optionally echo like the CLI.
  job.log = [&log, verbose](const std::string& line) {
    log.push_back(line);
    if (verbose) {
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
    }
  };
  py::gil_scoped_release nogil;
  job.opt.gpu_device = tmk::app::default_gpu_device(gpu);
  return tmk::app::run_route_job(std::move(job));
}

#define TM_ROUTE_ARGS                                                                                                          \
  py::arg("path_in"), py::arg("path_out") = py::none(), py::kw_only(), py::arg("time_s") = 120.0, py::arg("threads") = 8,       \
      py::arg("work") = 0L, py::arg("seed") = 1ULL, py::arg("view") = false, py::arg("view_host") = "0.0.0.0",                 \
      py::arg("view_port") = 8766, py::arg("gpu") = true, py::arg("kb") = true, py::arg("pitch_um") = 0.0,                      \
      py::arg("via_cost_mm") = 3.0, py::arg("optimize") = true, py::arg("rip_up") = true, py::arg("global_route") = false,     \
      py::arg("items_out") = py::none(), py::arg("json_out") = py::none(), py::arg("record") = py::none(),                    \
      py::arg("verbose") = false

py::dict route(const std::string& in, const std::optional<std::string>& out, double time_s, int threads, long work, std::uint64_t seed,
               bool view, const std::string& view_host, int view_port, bool gpu, const py::object& kb, double pitch_um, double via_cost_mm,
               bool optimize, bool rip_up, bool global_route, const std::optional<std::string>& items_out,
               const std::optional<std::string>& json_out, const std::optional<std::string>& record, bool verbose) {
  std::vector<std::string> log;
  const auto r = run_job(in, out, time_s, threads, work, seed, view, view_host, view_port, gpu, kb, pitch_um, via_cost_mm, optimize, rip_up,
                         global_route, items_out, json_out, record, verbose, log);
  py::dict d = to_python(r.summary).cast<py::dict>();
  py::list unrouted;
  for (const auto& u : r.result.unrouted) {
    py::dict e;
    e["net"] = u.net;
    e["a"] = u.a;
    e["b"] = u.b;
    unrouted.append(e);
  }
  d["unrouted"] = unrouted;
  d["exit_code"] = r.exit_code();
  d["viewer_url"] = r.viewer_url;
  d["log"] = log;
  return d;
}

py::dict emit_items(const std::string& in, const std::optional<std::string>& out, double time_s, int threads, long work, std::uint64_t seed,
                    bool view, const std::string& view_host, int view_port, bool gpu, const py::object& kb, double pitch_um,
                    double via_cost_mm, bool optimize, bool rip_up, bool global_route, const std::optional<std::string>& items_out,
                    const std::optional<std::string>& json_out, const std::optional<std::string>& record, bool verbose) {
  std::vector<std::string> log;
  const auto r = run_job(in, out, time_s, threads, work, seed, view, view_host, view_port, gpu, kb, pitch_um, via_cost_mm, optimize, rip_up,
                         global_route, items_out, json_out, record, verbose, log);
  return to_python(r.items).cast<py::dict>();
}

py::list drc(const std::string& path, double epsilon_um, bool unconnected) {
  tmk::drc::DrcReport rep;
  std::vector<std::string> copper;
  {
    py::gil_scoped_release nogil;
    const auto lb = tmk::io::read_board_file(path);
    const auto rules = tmk::io::read_design_rules(path);
    tmk::drc::DrcOptions opt;
    opt.epsilon = static_cast<tmk::Coord>(epsilon_um * 1000.0);
    rep = tmk::drc::run_drc(lb.board, rules, opt);
    for (int i = 0; i < lb.board.copper_count(); ++i) copper.push_back(lb.board.copper_name(i));
  }
  py::list out;
  auto add = [&](const tmk::drc::Violation& v) {
    py::dict d;
    d["type"] = v.type;
    d["severity"] = v.severity;
    d["description"] = v.description;
    py::list items;
    for (const auto& it : v.items) {
      py::dict e;
      e["description"] = it.description;
      e["x_mm"] = tmk::nm_to_mm(it.pos.x);
      e["y_mm"] = tmk::nm_to_mm(it.pos.y);
      items.append(e);
    }
    d["items"] = items;
    d["actual_mm"] = v.actual >= 0 ? py::object(py::float_(tmk::nm_to_mm(v.actual))) : py::none();
    d["required_mm"] = v.required >= 0 ? py::object(py::float_(tmk::nm_to_mm(v.required))) : py::none();
    d["layer"] = v.layer >= 0 && static_cast<std::size_t>(v.layer) < copper.size() ? py::object(py::str(copper[static_cast<std::size_t>(v.layer)]))
                                                                                 : py::none();
    out.append(d);
  };
  for (const auto& v : rep.violations) add(v);
  if (unconnected)
    for (const auto& v : rep.unconnected) add(v);
  return out;
}

}  // namespace

PYBIND11_MODULE(tracemaker, m) {
  m.doc() = "TraceMaker: placement-aware PCB autorouter for KiCad (Python bindings; coordinates in nm unless named *_mm)";
  m.attr("__version__") = std::string(tmk::version());

  py::class_<BoardSummary>(m, "Board", "Summary of a .kicad_pcb as TraceMaker reads it")
      .def_readonly("path", &BoardSummary::path)
      .def_readonly("version", &BoardSummary::version, "File format version (date)")
      .def_readonly("copper_layers", &BoardSummary::copper_layers, "Copper layer names in stack order")
      .def_readonly("net_names", &BoardSummary::net_names)
      .def_property_readonly("nets", &BoardSummary::nets, "Number of named nets")
      .def_readonly("footprints", &BoardSummary::footprints)
      .def_readonly("pads", &BoardSummary::pads)
      .def_readonly("tracks", &BoardSummary::tracks)
      .def_readonly("arcs", &BoardSummary::arcs)
      .def_readonly("vias", &BoardSummary::vias)
      .def_readonly("zones", &BoardSummary::zones)
      .def_readonly("outline_mm", &BoardSummary::outline_mm, "(width, height) of the Edge.Cuts bounding box, or None")
      .def_readonly("warnings", &BoardSummary::warnings)
      .def("__repr__", [](const BoardSummary& s) {
        return "<tracemaker.Board " + s.path + ": " + std::to_string(s.copper_layers.size()) + " copper layers, " + std::to_string(s.nets()) +
               " nets, " + std::to_string(s.footprints) + " footprints, " + std::to_string(s.pads) + " pads, " + std::to_string(s.tracks) +
               " tracks, " + std::to_string(s.vias) + " vias>";
      });

  m.def("read_board", &read_board, py::arg("path"), "Read a .kicad_pcb and return a Board summary");
  m.def("route", &route, TM_ROUTE_ARGS,
        "Route all unrouted connections (same code path as `tracemaker route`). Writes path_out when given and returns a dict "
        "with routed, connections, tracks, vias, seconds, expansions, pitch_mm, failures, unrouted, exit_code, viewer_url, log. "
        "The GIL is released while routing.");
  m.def("emit_items", &emit_items, TM_ROUTE_ARGS,
        "Route like route() and return the new copper as {'tracks': [...], 'vias': [...]} (the `route --emit-items` layout: "
        "layer and net by name, coordinates in nm)");
  m.def("drc", &drc, py::arg("path"), py::arg("epsilon_um") = 0.5, py::arg("unconnected") = true,
        "Run TraceMaker's DRC on a board with its KiCad design rules; one dict per violation (KiCad type names), "
        "followed by the unconnected items when `unconnected` is true");
}
