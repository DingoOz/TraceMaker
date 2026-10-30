// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/route_job.hpp"

#include <cstdlib>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "advice/advice.hpp"
#include "analysis/return_path.hpp"
#include "advice/advisor.hpp"
#include "advice/stuck_report.hpp"
#include "core/version.hpp"
#include "crules/critical.hpp"
#include "crules/engine.hpp"
#include "crules/names.hpp"
#include "drc/copper.hpp"
#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "drc/rule_engine.hpp"
#include "geom/clip.hpp"
#include "route/diff_pair.hpp"
#include "route/layer_limits.hpp"
#include "gpu/device.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "learn/knowledge_base.hpp"
#include "passes/pour.hpp"
#include "passes/stitch.hpp"
#include "passes/style.hpp"
#include "passes/widen.hpp"
#include "report/render.hpp"
#include "report/run_report.hpp"
#include "server/messages.hpp"
#include "server/replay_log.hpp"
#include "server/viewer_server.hpp"

namespace tmk::app {
namespace {

__attribute__((format(printf, 1, 2))) std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  const int n = std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  if (n < 0) return {};
  if (static_cast<std::size_t>(n) < sizeof buf) return std::string(buf, static_cast<std::size_t>(n));
  std::string s(static_cast<std::size_t>(n) + 1, '\0');
  va_start(ap, f);
  std::vsnprintf(s.data(), s.size(), f, ap);
  va_end(ap);
  s.resize(static_cast<std::size_t>(n));
  return s;
}

// Records router events (JSON lines with a time stamp) for replay, e.g. comparison videos. A path ending in
// ".zst" is written zstd-compressed (server/replay_log.hpp, decision D45).
class FileSink final : public events::Sink {
 public:
  explicit FileSink(const std::string& path) : w_(path), t0_(std::chrono::steady_clock::now()) {}
  void publish(std::string json) override {
    if (json.size() < 2 || json.front() != '{') return;
    const bool stamped = json.rfind("{\"t\":", 0) == 0;  // already time-stamped (buffered portfolio replay)
    const bool keyframe = !stamped && server::message_type(json) == "board";  // snapshots are never buffered
    std::lock_guard<std::mutex> lk(m_);
    if (stamped) {
      w_.write_line(json);
    } else {
      std::ostringstream t;
      t << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
      w_.write_line("{\"t\":" + t.str() + "," + json.substr(1));
    }
    if (keyframe) w_.end_frame();  // the snapshot is a frame of its own: a reader can start from it
  }
  bool wants_transient() const override { return false; }

 private:
  server::ReplayWriter w_;
  std::chrono::steady_clock::time_point t0_;
  std::mutex m_;
};

nlohmann::json items_json(const model::Board& b, const route::RouteResult& res) {
  // New copper for the KiCad plugin: layer and net by name, coordinates in nm.
  nlohmann::json it{{"tracks", nlohmann::json::array()}, {"vias", nlohmann::json::array()}};
  for (const auto& t : res.tracks)
    it["tracks"].push_back({{"start", {t.a.x, t.a.y}}, {"end", {t.b.x, t.b.y}}, {"width", t.width}, {"layer", b.copper_name(t.layer)},
                            {"net", b.nets[static_cast<std::size_t>(t.net)].name}});
  for (const auto& v : res.vias)
    it["vias"].push_back({{"position", {v.pos.x, v.pos.y}}, {"diameter", v.size}, {"drill", v.drill}, {"top", b.copper_name(v.layer_top)},
                          {"bottom", b.copper_name(v.layer_bottom)}, {"net", b.nets[static_cast<std::size_t>(v.net)].name}});
  return it;
}

// Work profile (doc 10 §2, D88), per phase and in total.
nlohmann::json work_json(const route::WorkProfile& w) {
  auto counts = [](const route::WorkCounts& c) {
    return nlohmann::json{{"expansions", c.expansions}, {"flood_cells", c.flood_cells}, {"searches", c.searches},
                          {"cell_checks", c.cell_checks}, {"fixed_checks", c.fixed_checks}, {"field_cells", c.field_cells},
                          {"fields", c.fields},         {"commits", c.commits},         {"rips", c.rips}};
  };
  nlohmann::json j;
  for (int p = 0; p < route::kWorkPhases; ++p) j[route::work_phase_name(static_cast<route::WorkPhase>(p))] = counts(w.phase[static_cast<std::size_t>(p)]);
  j["total"] = counts(w.total());
  return j;
}

// KiCad reads a board's rules (net classes, custom rules) from the project files beside it, so `route -o OUT` copies the input's
// .kicad_pro and .kicad_dru to OUT's name when OUT has none (never over existing ones; not for temporary outputs).
void copy_project_files(const std::string& in, const std::string& out) {
  namespace fs = std::filesystem;
  if (out.empty() || fs::path(out).extension() != ".kicad_pcb") return;
  std::error_code ec;
  if (fs::equivalent(in, out, ec)) return;
  for (const char* ext : {".kicad_pro", ".kicad_dru"}) {
    const fs::path src = fs::path(in).replace_extension(ext), dst = fs::path(out).replace_extension(ext);
    if (fs::exists(src, ec) && !fs::exists(dst, ec)) fs::copy_file(src, dst, ec);
  }
}
}  // namespace

std::vector<route::BusGroup> resolve_buses(const model::Board& b, const model::DesignRules& rules, const std::vector<std::string>& specs,
                                           bool detect, double skew_mm, std::vector<std::string>* notes) {
  std::vector<route::BusGroup> out;
  std::vector<int> owner(b.nets.size(), -1);
  auto claim = [&](const route::BusGroup& g) {
    for (const auto n : g.members)
      if (owner[static_cast<std::size_t>(n)] < 0) owner[static_cast<std::size_t>(n)] = static_cast<int>(out.size());
  };
  for (const auto& spec : specs) {
    auto g = route::bus_from_option(b, spec);
    for (const auto n : g.members)
      if (owner[static_cast<std::size_t>(n)] >= 0 && notes)
        notes->push_back("warning: net " + b.nets[static_cast<std::size_t>(n)].name + " is in bus " + out[static_cast<std::size_t>(owner[static_cast<std::size_t>(n)])].name +
                         " and " + g.name + "; it is routed with " + out[static_cast<std::size_t>(owner[static_cast<std::size_t>(n)])].name);
    claim(g);
    out.push_back(std::move(g));
  }
  if (detect)
    for (auto& g : route::detect_buses(b)) {
      if (std::any_of(g.members.begin(), g.members.end(), [&](model::NetId n) { return owner[static_cast<std::size_t>(n)] >= 0; })) continue;
      claim(g);
      out.push_back(std::move(g));
    }
  const drc::RuleEngine re(b, rules);
  for (auto& g : out) {
    if (const auto s = route::bus_rule_skew(re, g)) {
      g.skew_limit = *s;
      g.skew_source = "rule";
    } else if (g.source == "option" && skew_mm > 0) {
      g.skew_limit = static_cast<Coord>(skew_mm * 1e6);
      g.skew_source = "option";
    }
  }
  return out;
}

nlohmann::json bus_report(const model::Board& b, const model::DesignRules& rules, const std::vector<std::uint8_t>& joined, const route::BusGroup& g,
                          std::string* line) {
  const auto st = route::measure_bus(b, route::via_heights(b, rules), b.tracks, b.arcs, b.vias, joined, g);
  auto layer_names = [&](model::LayerMask m) {
    nlohmann::json a = nlohmann::json::array();
    for (int l = 0; l < b.copper_count(); ++l)
      if (m & model::layer_bit(l)) a.push_back(b.copper_name(l));
    return a;
  };
  nlohmann::json members = nlohmann::json::array();
  for (const auto& m : st.members)
    members.push_back({{"net", b.nets[static_cast<std::size_t>(m.net)].name}, {"routed", m.routed}, {"followed", m.followed},
                       {"length_mm", m.length / 1e6}, {"via_length_mm", m.via_length / 1e6}, {"vias", m.vias}, {"layers", layer_names(m.layers)}});
  nlohmann::json j{{"name", g.name}, {"source", g.source}, {"members", members}, {"nets", st.members.size()}, {"routed", st.routed},
                   {"followed", st.followed}, {"left", st.left}, {"reference_vias", st.ref_vias}, {"reference_layers", layer_names(st.ref_layers)},
                   {"vias_min", st.vias_min}, {"vias_max", st.vias_max}, {"length_min_mm", st.length_min / 1e6},
                   {"length_max_mm", st.length_max / 1e6}, {"skew_mm", st.skew() / 1e6}};
  // Within the limit only when every member is routed: an unrouted member has no length to compare.
  const bool all = st.routed == static_cast<int>(st.members.size());
  if (g.skew_limit > 0) {
    j["skew_limit_mm"] = nm_to_mm(g.skew_limit);
    j["skew_source"] = g.skew_source;
    j["within_skew"] = all && st.skew() <= static_cast<double>(g.skew_limit);
  } else {
    j["skew_limit_mm"] = nullptr;
    j["within_skew"] = nullptr;
  }
  if (line) {
    *line = fmt("%s (%s): %d/%zu nets routed, %d on the bus's layers, %d left the bus, vias %d-%d, length %.2f-%.2f mm, skew %.3f mm", g.name.c_str(),
                g.source.c_str(), st.routed, st.members.size(), st.followed, st.left, st.vias_min, st.vias_max, st.length_min / 1e6, st.length_max / 1e6,
                st.skew() / 1e6);
    if (g.skew_limit > 0)
      *line += fmt(" (limit %.3f mm from the %s: %s)", nm_to_mm(g.skew_limit), g.skew_source.c_str(),
                   !all ? "not all routed" : st.skew() <= static_cast<double>(g.skew_limit) ? "within" : "OVER");
  }
  return j;
}

int default_gpu_device(bool use_gpu) {
  if (!use_gpu) return -1;
  const auto devs = gpu::list_devices();
  return devs.empty() ? -1 : devs.front().index;
}

RouteJobResult run_route_job(RouteJob job) {
  const auto t_job = std::chrono::steady_clock::now();
  auto log = [&](const std::string& line) {
    if (job.log) job.log(line);
  };
  auto& opt = job.opt;
  auto lb = io::read_board_file(job.in);
  if (job.reroute) {
    // Strip the unlocked tracks and vias from the document and read it again (the editor changes the document,
    // not the model).
    int removed = 0;
    {
      io::BoardEditor ed(lb, opt.seed);
      for (std::size_t i = 0; i < lb.board.tracks.size(); ++i)
        if (!lb.board.tracks[i].locked) ed.remove_track(i), ++removed;
      for (std::size_t i = 0; i < lb.board.vias.size(); ++i)
        if (!lb.board.vias[i].locked) ed.remove_via(i), ++removed;
      const std::string tmp = (job.out.empty() ? (std::filesystem::temp_directory_path() / "tracemaker-reroute.kicad_pcb").string() : job.out + ".reroute-tmp.kicad_pcb");
      ed.save(tmp);
      lb = io::read_board_file(tmp);
      std::filesystem::remove(tmp);
    }
    log(fmt("reroute: %d unlocked tracks and vias removed", removed));
  }
  // The document after edits, parsed again, so the model and the node ids match what will be written.
  auto reparse = [&] {
    lb.doc = sexpr::Document::parse(lb.doc.write());
    lb.board = io::read_board(lb.doc);
  };
  if (job.copper_layers > 0 && job.copper_layers != lb.board.copper_count()) {
    const int had = lb.board.copper_count();
    std::string why;
    std::vector<std::string> notes;
    io::BoardEditor ed(lb, opt.seed);
    if (!ed.set_copper_layer_count(job.copper_layers, &why, &notes))
      throw std::invalid_argument(fmt("--copper-layers %d: %s", job.copper_layers, why.c_str()));
    reparse();
    log(fmt("copper layers: %d -> %d", had, lb.board.copper_count()));
    for (const auto& n : notes) log("  " + n);
  }
  if (!job.no_tracks_on.empty() || !job.layer_costs.empty()) {
    const auto& bd = lb.board;
    const auto lim = route::layer_limits(bd, job.no_tracks_on, job.layer_costs);
    opt.no_track_layers = lim.no_track_layers;
    opt.layer_cost_pm = lim.layer_cost_pm;
    std::string names;
    int stranded = 0;  // pads with a net whose copper lies only on layers without tracks
    for (int l = 0; l < bd.copper_count(); ++l)
      if (lim.no_track_layers & model::layer_bit(l)) names += (names.empty() ? "" : ", ") + bd.copper_name(l);
    for (const auto& p : bd.pads) stranded += p.net != 0 && p.copper != 0 && (p.copper & ~lim.no_track_layers) == 0;
    if (!names.empty()) log("no new tracks on: " + names);
    for (int l = 0; l < bd.copper_count() && !lim.layer_cost_pm.empty(); ++l)
      if (lim.layer_cost_pm[static_cast<std::size_t>(l)] != 1000)
        log(fmt("track cost on %s: x%.3g", bd.copper_name(l).c_str(), lim.layer_cost_pm[static_cast<std::size_t>(l)] / 1000.0));
    if (stranded) log(fmt("warning: %d pads lie only on layers that take no tracks; their connections stay unrouted", stranded));
  }
  auto rules = io::read_design_rules(job.in);  // component rules may add net classes (in memory only)
  // Zones without a fill (a generated board, or one saved before filling; doc 05 §54): left unfilled, the router does not see the planes,
  // so a plane net's pads are joined by tracks round the board (1,773 mm of GND and +54V track, some on the other net's plane layer, on
  // a 1,194-connection WholeBoard board) and fine-pitch escapes fight them. Filled in memory as KiCad will fill them and routed with soft
  // zones, the pads drop a via into their plane. The output keeps the zones as written: KiCad fills them.
  if (job.fill_planes) {
    std::vector<std::size_t> empty;
    for (std::size_t i = 0; i < lb.board.zones.size(); ++i) {
      const auto& z = lb.board.zones[i];
      if (!z.rule_area && !z.teardrop && z.net > 0 && z.copper != 0 && z.fills.empty()) empty.push_back(i);
    }
    if (!empty.empty()) {
      const auto filled = drc::refill_zones(lb.board, rules);
      for (const auto& w : filled.warnings) log("warning: " + w);
      for (const std::size_t i : empty) {
        lb.board.zones[i].fills = filled.board.zones[i].fills;
        double area = 0;
        for (const auto& [l, ring] : lb.board.zones[i].fills) area += std::abs(geom::area(ring));
        const auto& z = lb.board.zones[i];
        log(fmt("planes: zone %s (%s on %s) has no fill: filled in memory for routing, %.0f mm2", z.name.empty() ? "(unnamed)" : z.name.c_str(),
                lb.board.nets[static_cast<std::size_t>(z.net)].name.c_str(), z.layers.empty() ? "?" : z.layers.front().c_str(), area / 1e12));
      }
      if (!opt.soft_zones) log("planes: zone fills are soft for this run (--soft-zones): plane nets drop vias into their planes, other nets route through");
      opt.soft_zones = true;
    }
  }
  // Generated pours (doc 05 §53): added to the document (so they are written), filled in memory for the router, routed with soft
  // zones. Their fills in the output are redone for the final copper before writing.
  std::vector<std::pair<std::size_t, passes::PourPlan>> pours;  // (index in lb.board.zones, plan)
  if (!job.pours.empty()) {
    std::vector<std::string> notes;
    auto plans = passes::plan_pours(lb.board, rules, job.pours, &notes);
    for (const auto& n : notes) log(n);
    if (!plans.empty()) {
      io::BoardEditor ed(lb, opt.seed ^ 0x9047u);
      for (auto& p : plans) {
        p.zone.uuid = ed.next_uuid();
        ed.add_zone(p.zone);
      }
      reparse();
      const auto filled = drc::refill_zones(lb.board, rules);
      for (auto& p : plans)
        for (std::size_t i = 0; i < lb.board.zones.size(); ++i)
          if (lb.board.zones[i].uuid == p.zone.uuid) {
            lb.board.zones[i].fills = filled.board.zones[i].fills;
            double area = 0;
            for (const auto& [l, ring] : filled.board.zones[i].fills) area += std::abs(geom::area(ring));
            log(fmt("pour: %s, %.0f mm2 filled before routing", p.reason.c_str(), area / 1e12));
            pours.emplace_back(i, std::move(p));
          }
      if (!opt.soft_zones) log("pour: zone fills are soft for this run (--soft-zones), so other nets route through the pour");
      opt.soft_zones = true;
    }
  }
  if (opt.via_in_pad && (rules.minimums.via_diameter <= 0 || rules.minimums.through_hole_diameter <= 0))
    log("warning: --via-in-pad: the board's minimum via diameter or drill could not be read, so no via in pad is placed");
  const std::string name = std::filesystem::path(job.in).filename().string();
  RouteJobResult out;
  std::unique_ptr<server::ViewerServer> server;
  if (job.view) {
    server::ServerOptions so;
    so.host = job.view_host.empty() ? server::default_host() : job.view_host;
    so.port = static_cast<std::uint16_t>(job.view_port);
    so.token = job.view_token;
    so.allow_open = job.view_insecure;
    so.embed = job.view_embed;
    server = std::make_unique<server::ViewerServer>(so);
    server->publish(server::board_snapshot_json(lb.board, name, job.view_freeze));
    out.viewer_url = server->url();
    log("live view: " + out.viewer_url);
    if (const auto notice = server->notice(); !notice.empty()) log("warning: " + notice);
    opt.sink = server.get();
  }
  std::unique_ptr<FileSink> recorder;
  if (!job.record.empty() && !job.view) {
    recorder = std::make_unique<FileSink>(job.record);
    recorder->publish(server::board_snapshot_json(lb.board, name));
    opt.sink = recorder.get();
    opt.buffer_events = true;
  }
  // Footprints against datasheet land patterns (issue #5, doc 05 §43): before anything is routed, because a wrong footprint makes the routing worthless.
  nlohmann::json footprint_summary;
  if (!job.footprint_refs.empty()) {
    auto lib = analysis::load_references(job.footprint_refs, job.footprint_trusted);
    lib.trusted.trust_all = job.footprint_trust_all;
    const auto fr = analysis::check_footprints(lb.board, lib, job.footprint_opt);
    log(fmt("footprint check: %d footprints against %d datasheet references", fr.checked, fr.references));
    for (const auto& l : analysis::footprint_check_lines(fr, false)) log("  " + l);
    footprint_summary = analysis::footprint_check_json(lb.board, fr);
    auto& fps = footprint_summary["footprints"];
    fps.erase(std::remove_if(fps.begin(), fps.end(), [](const nlohmann::json& f) { return f["issues"].empty(); }), fps.end());  // only what has something to say
    if (opt.sink) opt.sink->publish(analysis::footprint_check_message(fr));
    if (fr.errors > 0 && !job.footprint_warn_only)
      throw std::runtime_error(fmt("footprint check: %d error%s; fix the footprints, or route anyway with --footprint-warn-only", fr.errors, fr.errors == 1 ? "" : "s"));
  }
  // Knowledge base (failure memory T3): earlier failures on this board go first; variant choice by bandit.
  std::unique_ptr<learn::KnowledgeBase> kb;
  if (!job.kb_path.empty()) {
    kb = std::make_unique<learn::KnowledgeBase>(job.kb_path);
    if (!kb->ok()) kb.reset();
  }
  int conn_estimate = 0;
  for (const auto& n : lb.board.nets) conn_estimate += !n.name.empty();
  const auto feat = learn::features_of(lb.board, lb.doc.text(), conn_estimate);
  if (kb) {
    for (const auto& f : kb->failed_connections(feat.hash)) opt.priority.emplace_back(f.pad_a, f.pad_b);
    if (!opt.priority.empty()) log(fmt("knowledge base: %zu connections that failed before are routed first", opt.priority.size()));
  }
  // Component rules (doc 15 §5.5): generated keep-outs are added to an in-memory copy of the board the router
  // sees; the output is written from the original document, so the user's board never gains them.
  const crules::Mode cr_mode = crules::parse_mode(job.component_rules);
  // After-route passes (doc 05 §39): what the options and the component rules ask of them.
  if (!job.stitch_vias.empty() && job.stitch_vias != "planes" && job.stitch_vias != "return" && job.stitch_vias != "all")
    throw std::invalid_argument("--stitch-vias: expected planes, return or all, got '" + job.stitch_vias + "'");
  const bool stitch_planes = job.stitch_vias == "planes" || job.stitch_vias == "all";
  const bool stitch_return = job.stitch_vias == "return" || job.stitch_vias == "all";
  std::vector<passes::WidenOptions::Target> widen_targets;
  std::vector<passes::StitchOptions::ReturnNet> stitch_returns;
  // "PATTERN=VALUE": the nets whose name matches the pattern (KiCad wildcards), and the text after the last '='.
  auto pattern_nets = [&](const std::string& opt_name, const std::string& entry, std::string& value) {
    const auto eq = entry.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) throw std::invalid_argument(opt_name + ": expected PATTERN=VALUE, got '" + entry + "'");
    value = entry.substr(eq + 1);
    std::vector<model::NetId> nets;
    for (const auto& n : lb.board.nets)
      if (n.id > 0 && !n.name.empty() && model::wildcard_match(entry.substr(0, eq), n.name)) nets.push_back(n.id);
    if (nets.empty()) log("warning: " + opt_name + " " + entry + " matches no net");
    return nets;
  };
  for (const auto& e : job.widen_nets) {
    std::string v;
    const auto nets = pattern_nets("--widen-net", e, v);
    char* end = nullptr;
    const double mm = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !(mm > 0) || mm > 100) throw std::invalid_argument("--widen-net: bad width in '" + e + "' (millimetres)");
    for (const auto n : nets) widen_targets.push_back({n, static_cast<Coord>(std::llround(mm * 1e6)), "option"});
  }
  if (stitch_return)
    for (const auto& e : job.stitch_returns) {
      std::string v;
      const auto nets = pattern_nets("--stitch-return", e, v);
      const model::NetId plane = lb.board.net_by_name(v);
      if (plane == 0) throw std::invalid_argument("--stitch-return: no net named '" + v + "'");
      const double mm = job.stitch_distance_mm > 0 ? job.stitch_distance_mm : crules::builtin_catalogue().stitch_via_within_mm;
      for (const auto n : nets)
        if (n != plane) stitch_returns.push_back({n, plane, static_cast<Coord>(mm * 1e6), "option"});
    }
  model::Board with_rules;
  nlohmann::json cr_classes = nlohmann::json::array();
  const model::Board* route_board = &lb.board;
  if (cr_mode == crules::Mode::Off && !job.rules_override.empty()) {
    crules::load_overrides_file(job.rules_override, crules::builtin_catalogue());  // still validated: a broken file is an error
    log("warning: --rules-override " + job.rules_override + " has no effect with --component-rules off");
  }
  if (cr_mode != crules::Mode::Off) {
    const auto& cat = crules::builtin_catalogue();
    crules::Overrides ov;
    if (!job.rules_override.empty()) ov = crules::load_overrides_file(job.rules_override, cat);
    const auto det = crules::detect(lb.board, cat, job.rules_override.empty() ? nullptr : &ov);
    if (!job.rules_override.empty()) {
      log(fmt("component rules: %zu user override(s) from %s", det.override_entries.size(), job.rules_override.c_str()));
      for (const auto& u : det.override_unused) log("  warning: override matched nothing: " + u);
    }
    std::vector<std::string> skipped;
    const auto kos = crules::generate_keepouts(lb.board, cat, det, &skipped);
    log(fmt("component rules (%s): %zu instance(s), %zu generated keep-out(s)%s", job.component_rules.c_str(), det.instances.size(), kos.size(),
            cr_mode == crules::Mode::On ? "" : " (not applied: use --component-rules on)"));
    for (const auto& s : skipped) log("  keep-out not generated: " + s);
    // Events for the viewer and recordings (rule 7, doc 15 §7): what was detected and the keep-out polygons (nm).
    if (opt.sink) {
      for (const auto& in : det.instances)
        opt.sink->publish(nlohmann::json{{"type", "crules.detected"},
                                         {"category", cat.categories[static_cast<std::size_t>(in.category)].id},
                                         {"anchor", lb.board.footprints[static_cast<std::size_t>(in.anchor)].reference},
                                         {"confidence", in.confidence}}
                              .dump());
      for (const auto& k : kos) {
        nlohmann::json poly = nlohmann::json::array();
        for (const auto& q : k.zone.outline.front()) poly.push_back({q.x, q.y});
        opt.sink->publish(nlohmann::json{{"type", "crules.keepout"}, {"name", k.zone.name}, {"layers", k.zone.layers},
                                         {"applied", cr_mode == crules::Mode::On}, {"polygon", poly}}
                              .dump());
      }
    }
    // Net classes from impedance and width rules (P5, doc 15 §16): reported in every mode, used by the router with
    // --component-rules on.
    {
      const auto ev = crules::evaluate(lb.board, &rules, cat, det, cr_mode);
      const auto sc = crules::synthetic_net_classes(lb.board, rules, cat, det, ev);
      for (const auto& c : sc) {
        std::string nets;
        for (const auto n : c.nets) nets += (nets.empty() ? "" : ", ") + lb.board.nets[static_cast<std::size_t>(n)].name;
        log("  net class " + c.cls.name + " (" + c.rule + "): " + c.detail + "; nets " + nets + (cr_mode == crules::Mode::On ? "" : " (not applied: use --component-rules on)"));
        nlohmann::json nj = nlohmann::json::array();
        for (const auto n : c.nets) nj.push_back(lb.board.nets[static_cast<std::size_t>(n)].name);
        cr_classes.push_back({{"name", c.cls.name}, {"rule", c.rule}, {"track_width_mm", nm_to_mm(c.cls.track_width)},
                              {"diff_pair_gap_mm", c.cls.has_diff_pair_gap ? nm_to_mm(c.cls.diff_pair_gap) : 0.0}, {"nets", nj},
                              {"detail", c.detail}, {"applied", cr_mode == crules::Mode::On}});
      }
      if (cr_mode == crules::Mode::On && !sc.empty()) rules = crules::with_synthetic_classes(lb.board, std::move(rules), sc);
      // --widen-power: a width a width_for_current rule computed is a target in every mode (impedance widths are
      // not: a controlled-impedance track must not be wider than computed either).
      if (job.widen_power)
        for (const auto& c : sc)
          for (const auto& cg : cat.categories)
            for (const auto& r : cg.rules)
              if (r.id == c.rule && r.kind == "width_for_current")
                for (const auto n : c.nets) widen_targets.push_back({n, c.cls.track_width, c.rule});
    }
    // --stitch-vias return: nets bound to a reference_plane rule want a via of the plane net near each of their
    // vias (doc 15 §4; the rule's stitch_via_mm, else the catalogue's general high-speed distance).
    if (stitch_return)
      for (const auto& in : det.instances) {
        if (in.confidence < cat.apply) continue;
        for (const auto& r : cat.categories[static_cast<std::size_t>(in.category)].rules) {
          if (r.kind != "reference_plane" || in.disabled(r.id)) continue;
          const std::string plane_name = r.params.contains("plane_net") && r.params["plane_net"].is_string() ? r.params["plane_net"].get<std::string>() : "GND";
          model::NetId plane = 0;
          if (const crules::Role* pr = in.role(crules::lower(plane_name)); pr && !pr->nets.empty()) plane = pr->nets.front();
          for (const auto& n : lb.board.nets)  // else the net of that name (last hierarchy component), lowest id
            if (plane == 0 && n.id > 0 && crules::upper(crules::net_leaf(n.name)) == crules::upper(plane_name)) plane = n.id;
          const std::string src = r.id + " " + lb.board.footprints[static_cast<std::size_t>(in.anchor)].reference;
          if (plane == 0) {
            log("  stitching: " + src + " names plane net " + plane_name + ", which this board does not have");
            continue;
          }
          const double mm = job.stitch_distance_mm > 0 ? job.stitch_distance_mm
                            : r.params.contains("stitch_via_mm") && r.params["stitch_via_mm"].is_number() ? r.params["stitch_via_mm"].get<double>()
                            : cat.stitch_via_within_mm;
          for (const auto& role : r.applies_to)
            if (const crules::Role* x = in.role(role))
              for (const auto n : x->nets) {
                const bool seen = std::any_of(stitch_returns.begin(), stitch_returns.end(), [&](const auto& y) { return y.signal == n && y.plane == plane; });
                if (n > 0 && n != plane && !seen) stitch_returns.push_back({n, plane, static_cast<Coord>(mm * 1e6), src});
              }
        }
      }
    // USB2-02 (P3): route each detected USB 2.0 D+/D- pair coupled first (a soft preference: the router falls back
    // to single tracks). Only pairs bound to exactly one net each.
    if (cr_mode == crules::Mode::Soft || cr_mode == crules::Mode::On)
      for (const auto& p : crules::usb_pairs(lb.board, cat, det)) {
        if (std::find(opt.pair_nets.begin(), opt.pair_nets.end(), p) != opt.pair_nets.end()) continue;
        opt.pair_nets.push_back(p);
        // The interface's own intra-pair skew limit (USB2-02 max_intra_skew_mm), unless the user gave one for all pairs.
        Coord skew = 0;
        for (const auto& c : cat.categories)
          for (const auto& r : c.rules)
            if (r.id == "USB2-02" && r.params.contains("max_intra_skew_mm") && r.params["max_intra_skew_mm"].is_number())
              skew = static_cast<Coord>(r.params["max_intra_skew_mm"].get<double>() * 1e6);
        opt.pair_net_skew.resize(opt.pair_nets.size(), 0);
        opt.pair_net_skew.back() = skew;
        log("  differential pair (USB2-02): " + lb.board.nets[static_cast<std::size_t>(p.first)].name + " / " +
            lb.board.nets[static_cast<std::size_t>(p.second)].name);
      }
    if (cr_mode == crules::Mode::On && !kos.empty()) {
      with_rules = lb.board;
      for (const auto& k : kos) {
        with_rules.zones.push_back(k.zone);
        log("  keep-out " + k.zone.name + " on " + [&] {
          std::string l;
          for (const auto& n : k.zone.layers) l += (l.empty() ? "" : "+") + n;
          return l;
        }());
      }
      route_board = &with_rules;
    }
    if (!job.out.empty()) {
      std::string side = job.out;
      if (side.ends_with(".kicad_pcb")) side.resize(side.size() - 10);
      side += ".tracemaker.kicad_dru";
      std::ofstream(side) << crules::dru_sidecar(lb.board, cat, det);
      log("component rules: generated custom rules written to " + side);
    }
  }
  // Custom rules the router cannot apply (unreadable conditions, position-dependent disallow rules) are named
  // here, not only by `tracemaker drc`: the routed board may break them (rule 6).
  std::vector<std::string> engine_warnings;  // kept for the run report's list of what was not read
  {
    const drc::RuleEngine re(*route_board, rules);
    engine_warnings = re.warnings();
    for (const auto& w : engine_warnings) log("warning: " + w);
  }
  // Buses (doc 05 §38): the groups asked for, with the nets found for each, so a wrong pattern shows before routing.
  std::vector<route::BusGroup> bus_groups;
  if (!job.buses.empty() || job.detect_buses) {
    std::vector<std::string> notes;
    bus_groups = resolve_buses(*route_board, rules, job.buses, job.detect_buses, job.bus_skew_mm, &notes);
    for (const auto& n : notes) log(n);
    log(fmt("buses: %zu group%s", bus_groups.size(), bus_groups.size() == 1 ? "" : "s"));
    for (const auto& g : bus_groups) {
      std::string nets;
      for (const auto n : g.members) nets += (nets.empty() ? "" : " ") + lb.board.nets[static_cast<std::size_t>(n)].name;
      log(fmt("  bus %s (%s, %zu nets, %s): %s", g.name.c_str(), g.source.c_str(), g.members.size(),
              g.skew_limit > 0 ? fmt("skew limit %.3f mm from the %s", nm_to_mm(g.skew_limit), g.skew_source.c_str()).c_str() : "no skew limit", nets.c_str()));
      opt.buses.push_back({g.name, g.members, g.skew_limit});
    }
  }
  // KiCad 10 tuning profiles (doc 05 §58, D145). The router has one width per net. A single-ended profile's nets are routed at the widest
  // of the profile's widths (so their clearance holds at any of them) and narrowed afterwards, layer by layer, to the profile's width there:
  // narrower copper cannot break a clearance. A pair's gap would change with the width, so a differential profile's pairs are routed at the
  // width and gap of one layer and kept off the layers with another width or gap: a layer the class's SMD pads are on, then the geometry
  // most of the profile's layers share, then the first.
  nlohmann::json tuning_summary;
  std::map<model::NetId, std::vector<Coord>> tuning_widths;  // net -> profile width per copper index (0 = none)
  for (auto& c : rules.classes) {
    const model::TuningProfile* p = rules.profile_of(c);
    if (!p) continue;
    std::vector<const model::TuningLayer*> on;  // the profile's layers that the board has, with a width
    for (const auto& l : p->layers)
      if (l.width > 0 && lb.board.copper_index(l.layer) >= 0) on.push_back(&l);
    if (on.empty()) continue;
    const model::TuningLayer* from = on[0];
    nlohmann::json excluded = nlohmann::json::array();
    if (p->differential) {
      auto same = [](const model::TuningLayer* x, const model::TuningLayer* y) { return x->width == y->width && x->gap == y->gap; };
      auto uses = [&](const model::TuningLayer* x) { return std::count_if(on.begin(), on.end(), [&](const auto* y) { return same(x, y); }); };
      // Prefer the layers the class's SMD pads are on: a pair kept off its own pads' layer could not leave them.
      std::vector<bool> pad_layer(static_cast<std::size_t>(lb.board.copper_count()), false);
      for (const auto& pad : lb.board.pads)
        if (pad.net > 0 && std::popcount(pad.copper) == 1 && rules.class_for(lb.board.nets[static_cast<std::size_t>(pad.net)].name).name == c.name)
          pad_layer[static_cast<std::size_t>(std::countr_zero(pad.copper))] = true;
      auto on_pads = [&](const model::TuningLayer* x) { return pad_layer[static_cast<std::size_t>(lb.board.copper_index(x->layer))]; };
      for (const auto* l : on)
        if (std::pair(on_pads(l), uses(l)) > std::pair(on_pads(from), uses(from))) from = l;
      c.diff_pair_width = c.track_width = from->width;  // uncoupled pieces of a pair have the profile's width too
      c.tuning_pair_gap = from->gap;
      for (const auto* l : on) {
        if (same(l, from)) continue;
        model::CustomRule no_track{"tuning profile '" + p->name + "' on " + l->layer, "A.hasExactNetclass('" + c.name + "') && A.inDiffPair('*')", l->layer, "error", {}};
        no_track.constraints.push_back({"disallow", std::nullopt, std::nullopt, std::nullopt, {"track"}});
        opt.extra_rules.push_back(no_track);
        excluded.push_back({{"layer", l->layer}, {"width_mm", nm_to_mm(l->width)}, {"gap_mm", nm_to_mm(l->gap)}});
      }
    } else {
      for (const auto* l : on)
        if (l->width > from->width) from = l;
      // KiCad's width rule holds every track of the class, a pair's halves too.
      c.diff_pair_width = c.track_width = from->width;
      std::vector<Coord> widths(static_cast<std::size_t>(lb.board.copper_count()), 0);
      for (const auto* l : on) widths[static_cast<std::size_t>(lb.board.copper_index(l->layer))] = std::max(l->width, rules.minimums.track_width);
      for (std::size_t n = 1; n < lb.board.nets.size(); ++n)
        if (rules.class_for(lb.board.nets[n].name).name == c.name) tuning_widths[static_cast<model::NetId>(n)] = widths;
    }
    // The profile's width is also KiCad's minimum there: no neck-down for the class's nets.
    for (std::size_t n = 1; n < lb.board.nets.size(); ++n)
      if (rules.class_for(lb.board.nets[n].name).name == c.name) opt.no_neck_nets.push_back(lb.board.nets[n].name);
    nlohmann::json layers = nlohmann::json::array();
    for (const auto* l : on) layers.push_back({{"layer", l->layer}, {"width_mm", nm_to_mm(l->width)}, {"gap_mm", nm_to_mm(l->gap)}});
    log(fmt("tuning profile %s (net class %s, %s): routed at %.3f mm (the width on %s)%s", p->name.c_str(), c.name.c_str(),
            p->differential ? "differential pairs" : "single-ended", nm_to_mm(from->width), from->layer.c_str(),
            p->differential ? (excluded.empty() ? "" : fmt("; pairs kept off %zu layer%s with another width or gap", excluded.size(), excluded.size() == 1 ? "" : "s").c_str())
                            : "; narrowed to each layer's width after routing"));
    if (tuning_summary.is_null()) tuning_summary = nlohmann::json::array();
    tuning_summary.push_back({{"net_class", c.name}, {"profile", p->name}, {"differential", p->differential}, {"route_width_mm", nm_to_mm(from->width)},
                              {"route_width_from", from->layer}, {"pair_gap_mm", p->differential ? nm_to_mm(from->gap) : 0.0}, {"layers", layers},
                              {"layers_without_tracks", excluded}});
  }
  for (const auto& c : rules.classes)
    if (!c.tuning_profile.empty() && !rules.profile_of(c))
      log("warning: net class " + c.name + " names tuning profile " + c.tuning_profile + ", which the project does not define (KiCad: missing_tuning_profile)");
  // Critical nets (issue #24, doc 05 §44): the names the user gave and, with --critical-auto, the nets the component rules' roles make critical.
  std::vector<crules::CriticalNet> critical;
  {
    const bool si = job.si_rules;
    const bool want_auto = job.critical_auto || si;
    const bool want_first = job.critical_first || si;
    const double via_mult = job.critical_via_given || !si ? job.critical_via_mult : 3.0;
    const double split = job.split_cost_given || !si ? job.split_cost : 4.0;
    if (!job.critical_nets.empty() || want_auto) {
      std::map<model::NetId, crules::CriticalNet> found;
      for (const auto& n : lb.board.nets) {
        if (n.id <= 0 || n.name.empty()) continue;
        for (const auto& pat : job.critical_nets)
          if (model::wildcard_match(pat, n.name)) {
            found[n.id] = {n.id, 1, "named (--critical-nets " + pat + ")"};
            break;
          }
      }
      if (want_auto) {
        const auto& cat = crules::builtin_catalogue();
        crules::Overrides ov;  // the user's override file (doc 15 §6.3) changes what a part is, and so which nets are critical
        if (!job.rules_override.empty()) ov = crules::load_overrides_file(job.rules_override, cat);
        const auto det = crules::detect(lb.board, cat, job.rules_override.empty() ? nullptr : &ov);
        auto pairs = opt.pair_nets;
        for (const auto& p : route::named_pairs(lb.board, drc::RuleEngine(lb.board, rules)))
          if (std::find(pairs.begin(), pairs.end(), p) == pairs.end()) pairs.push_back(p);
        for (const auto& c : crules::critical_nets(lb.board, cat, det, pairs))
          if (!found.count(c.net)) found[c.net] = c;
      }
      for (const auto& [id, c] : found) critical.push_back(c);
      std::stable_sort(critical.begin(), critical.end(), [](const crules::CriticalNet& a, const crules::CriticalNet& c) { return a.tier < c.tier; });
    }
    for (const auto& c : critical) opt.critical.emplace_back(lb.board.nets[static_cast<std::size_t>(c.net)].name, c.tier);
    opt.critical_first = want_first && !critical.empty();
    opt.critical_via_mult = via_mult;
    opt.split_cost = critical.empty() ? 0.0 : split;
    opt.split_hard = job.split_hard && !critical.empty();
    if ((opt.split_cost > 0 || opt.split_hard) && !critical.empty()) {  // zones without fills are refilled once here, not by every variant
      bool unfilled = false;
      for (const auto& z : route_board->zones) unfilled = unfilled || (!z.rule_area && z.net > 0 && z.fills.empty());
      if (unfilled) opt.reference_fills = std::make_shared<const model::Board>(drc::refill_zones(*route_board, rules).board);
    }
    if (!critical.empty() || job.critical_auto || si || !job.critical_nets.empty()) {
      std::string lines;
      for (int tier = 1; tier <= 3; ++tier) {
        std::string names;
        int n = 0;
        for (const auto& c : critical)
          if (c.tier == tier && ++n <= 8) names += (names.empty() ? "" : ", ") + lb.board.nets[static_cast<std::size_t>(c.net)].name;
        const int total = static_cast<int>(std::count_if(critical.begin(), critical.end(), [&](const crules::CriticalNet& c) { return c.tier == tier; }));
        if (total > 0) lines += fmt("%s tier %d: %d (%s%s)", lines.empty() ? "" : "; ", tier, total, names.c_str(), total > 8 ? ", ..." : "");
      }
      log(fmt("critical nets: %zu%s%s", critical.size(), lines.empty() ? "" : " - ", lines.c_str()));
      log(fmt("  routed first: %s; via cost x%.3g; over a plane void: %s", opt.critical_first ? "yes" : "no", opt.critical_via_mult,
              opt.split_hard ? "not allowed" : opt.split_cost > 0 ? fmt("+%.3g pitches per lattice step", opt.split_cost).c_str() : "free"));
    }
  }
  if (!job.warm_pcb.empty()) {  // copper of an earlier solution (looser rules, legalised): the router keeps what is legal
    const auto wb = io::read_board_file(job.warm_pcb);
    for (const auto& t : wb.board.tracks)
      if (!t.locked) opt.warm_tracks.push_back(t);
    for (const auto& v : wb.board.vias) opt.warm_vias.push_back(v);
    opt.state_recheck = true;
    log(fmt("warm start from %s: %zu tracks, %zu vias", job.warm_pcb.c_str(), opt.warm_tracks.size(), opt.warm_vias.size()));
  }
  auto& res = out.result;
  auto route_t0 = std::chrono::steady_clock::now();  // --time covers one routing run, the refill repair after it included
  std::vector<int> ran;
  int best_index = 0;
  std::string best_name;
  nlohmann::json variant_rows = nlohmann::json::array();
  // The variant set is a setting, never derived from the thread count when a work budget makes the run
  // deterministic: then `--threads` only changes how fast the same variants finish (requirement N3, D47).
  const int threads = std::max(1, job.threads);
  // An explicit --variants may go beyond the eight designed variants: the extras are seeded diversifications
  // (doc 16: on hard dense breakouts the winning variant is spread over the portfolio, so more seeds find more).
  const int variants = std::clamp(job.variants > 0 ? job.variants : opt.work_budget > 0 ? route::portfolio_size() : threads, 1,
                                  job.variants > 0 ? 64 : route::portfolio_size());
  route::WorkProfile all_work;  // summed over the portfolio's variants (and over the advised runs)

  // Advice (doc 17): a file given with --advice is applied from the first run on. Its hints go to advised variants next to the
  // unadvised ones, so the result can only be better than without it.
  nlohmann::json advice_summary;  // null without advice
  auto announce_advice = [&](const advice::Applied& ap, const char* origin) {
    log(fmt("advice (%s): %s%s", origin, ap.set.source.c_str(), ap.report["note"].get<std::string>().empty() ? "" : (" - " + ap.report["note"].get<std::string>()).c_str()));
    for (const auto& h : ap.report["hints"]) {
      log("  " + h["text"].get<std::string>());
      if (opt.sink) opt.sink->publish(nlohmann::json{{"type", "advice.hint"}, {"kind", h["kind"]}, {"text", h["text"]}, {"origin", origin}}.dump());
    }
    for (const auto& w : ap.warnings) log("  warning: " + w);
  };
  if (!job.advice.empty()) {
    const auto adv = advice::load_advice_file(job.advice);  // refused with the file and the entry on any problem
    auto ap = advice::apply_advice(adv, lb.board);
    if (job.advice_variants > 0) ap.set.variants = std::clamp(job.advice_variants, 1, advice::kMaxVariants);
    announce_advice(ap, "file");
    opt.advice = ap.set;
    advice_summary = {{"file", ap.report}, {"warnings", ap.warnings}};
  }
  // Variants the knowledge base knows are the unadvised ones; advised copies are not recorded there.
  auto unadvised_only = [](std::vector<int> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](int i) { return i >= route::kAdvisedIndex; }), v.end());
    return v;
  };
  // One routing run under the options `o`: the portfolio when several variants run or advice adds some, else one router.
  struct Run {
    route::RouteResult res;
    std::vector<int> ran;
    int best_index = 0;
    std::string best_name;
    nlohmann::json rows = nlohmann::json::array();
    route::WorkProfile work;
    int unadvised_best = -1, advised_best = -1;  // routed counts of the best variant of each kind (-1: none ran)
  };
  auto run_once = [&](const route::RouterOptions& o, int round, bool use_kb) {
    Run r;
    // The round-based options (--relax, --rounds, --round-work, --ramp-from) are implemented in the portfolio; given with one
    // variant they were silently ignored, so they run a portfolio of one (M17: the C1 experiment found it).
    const bool rounds_asked = o.relax || o.rounds > 1 || o.round_work > 0 || o.ramp_from_mm > 0;
    if (variants > 1 || o.advice.active() || rounds_asked) {
      // --relax budgets by rounds, not by --work (soak 2026-10-25: a --work run took the full --time); say so instead of surprising.
      if (round <= 0 && o.relax && o.work_budget > 0 && o.round_work <= 0)
        log("relax: rounds of 20 M work until the board is complete; --work does not limit them (bound with --round-work, --rounds or --time)");
      std::vector<int> pick;
      if (use_kb && kb && variants < route::portfolio_size()) pick = kb->choose_variants(feat, route::portfolio_size(), variants, o.seed);
      if (const char* e = std::getenv("TM_PICK_VARIANTS")) {  // debugging: run these variant indices of the list only (comma separated)
        pick.clear();
        for (const char* q = e; *q;) {
          char* end = nullptr;
          pick.push_back(static_cast<int>(std::strtol(q, &end, 10)));
          q = *end ? end + 1 : end;
        }
      }
      log(fmt("portfolio: %d variants%s on %d thread%s", variants, o.advice.active() ? fmt(" + %d advised", std::min(o.advice.variants, variants)).c_str() : "",
              std::min(threads, variants), std::min(threads, variants) == 1 ? "" : "s"));
      auto pr = route::route_portfolio(*route_board, rules, o, variants, pick, threads);
      for (std::size_t i = 0; i < pr.variants.size(); ++i) {
        log(fmt("  variant %d %-30s routed %d in %.1f s%s", pr.indices[i], pr.variants[i].c_str(), pr.routed[i], pr.seconds[i],
                static_cast<int>(i) == pr.best_variant ? "  <- best" : ""));
        nlohmann::json row{{"variant", pr.indices[i]}, {"name", pr.variants[i]}, {"routed", pr.routed[i]}, {"seconds", pr.seconds[i]}};
        if (round > 0) row["round"] = round;
        r.rows.push_back(std::move(row));
        int& best = pr.indices[i] >= route::kAdvisedIndex ? r.advised_best : r.unadvised_best;
        best = std::max(best, pr.routed[i]);
      }
      r.ran = pr.indices;
      r.best_index = pr.indices[static_cast<std::size_t>(pr.best_variant)];
      r.best_name = pr.variants[static_cast<std::size_t>(pr.best_variant)];
      r.res = std::move(pr.best);
      for (const auto& w : pr.work)
        for (int p = 0; p < route::kWorkPhases; ++p) r.work.phase[static_cast<std::size_t>(p)] += w.phase[static_cast<std::size_t>(p)];
    } else {
      r.res = route::Router(*route_board, rules, o).run();
      r.ran = {0};
      r.work = r.res.work;
    }
    return r;
  };
  {
    Run first = run_once(opt, 0, true);
    res = std::move(first.res);
    ran = std::move(first.ran);
    best_index = first.best_index;
    best_name = std::move(first.best_name);
    variant_rows = std::move(first.rows);
    all_work = first.work;
    if (!advice_summary.is_null()) {
      advice_summary["unadvised_best_routed"] = first.unadvised_best;
      advice_summary["advised_best_routed"] = first.advised_best;
      advice_summary["winner_advised"] = best_index >= route::kAdvisedIndex;
      log(fmt("advice: best unadvised variant routed %d, best advised %d%s", first.unadvised_best, first.advised_best,
              best_index >= route::kAdvisedIndex ? " (an advised variant won)" : ""));
    }
  }
  // The advisor (doc 17 §5): consulted only when connections are left. It gets the stuck report, answers with an advice file,
  // and the board is routed again with it; the better result of the two stands. A missing, slow or nonsensical advisor is a
  // warning and the run goes on as it was (rule 5).
  nlohmann::json advisor_rounds = nlohmann::json::array();
  if (!job.advisor.empty() || !job.stuck_report_out.empty()) {
    const int total_rounds = job.advisor.empty() ? 1 : std::max(job.advisor_rounds, 1);
    std::string advice_base = job.advice_out;
    if (advice_base.empty() && !job.out.empty()) {
      advice_base = job.out;
      if (advice_base.ends_with(".kicad_pcb")) advice_base.resize(advice_base.size() - 10);
      advice_base += ".advice.json";
    }
    nlohmann::json previous = advice_summary.is_null() ? nlohmann::json() : advice_summary["file"];
    for (int round = 1; round <= total_rounds; ++round) {
      nlohmann::json rj{{"round", round}, {"routed_before", res.routed}, {"connections", res.connections}};
      advice::StuckInput si;
      si.board = &lb.board;
      si.rules = &rules;
      si.result = &res;
      si.board_name = name;
      si.seconds = res.seconds;
      si.variants = variant_rows;
      si.previous_advice = previous;
      si.redact = job.advisor_redact;
      si.soft_zones = opt.soft_zones;
      si.vias_off_pads_below_mm = opt.vias_off_pads ? opt.vias_off_pads_below_mm : 0.0;
      const auto report = advice::build_stuck_report(si);
      if (!job.stuck_report_out.empty() && round == 1) {
        std::ofstream(job.stuck_report_out) << report.json.dump(1);
        log("stuck report written to " + job.stuck_report_out);
      }
      if (job.advisor.empty()) break;
      if (res.routed >= res.connections) {
        log("advisor: every connection is routed, it is not consulted");
        rj["skipped"] = "complete";
        advisor_rounds.push_back(rj);
        break;
      }
      if (opt.sink) opt.sink->publish(server::stage("advisor", "begin", fmt("%d connections open", res.connections - res.routed)));
      log(fmt("advisor: %d of %d connections are open; consulting it (limit %.0f s%s)", res.connections - res.routed, res.connections, job.advisor_timeout_s,
              job.advisor_redact ? ", names redacted" : ""));
      const auto c = advice::consult_advisor({job.advisor, job.advisor_timeout_s}, report.json, report.aliases, "the advisor's answer");
      rj["advisor"] = c.summary;
      auto stop = [&](const std::string& why) {
        log("warning: " + why + "; the run goes on without it");
        rj["error"] = why;
        advisor_rounds.push_back(rj);
        if (opt.sink) {
          opt.sink->publish(nlohmann::json{{"type", "advice.rejected"}, {"why", why}}.dump());
          opt.sink->publish(server::stage("advisor", "end", why));
        }
      };
      if (!c.ok) {
        stop(c.error);
        break;
      }
      advice::Applied ap;
      try {
        ap = advice::apply_advice(c.advice, lb.board);
      } catch (const std::exception& e) {
        stop(std::string("the advisor's advice was refused: ") + e.what());
        break;
      }
      ap.set.source = "the advisor";
      if (job.advice_variants > 0) ap.set.variants = std::clamp(job.advice_variants, 1, advice::kMaxVariants);
      // Saved with the real names (aliases undone), so `--advice FILE` repeats this run.
      std::string saved;
      if (!advice_base.empty()) {
        saved = total_rounds > 1 ? advice_base.substr(0, advice_base.size() - 5) + "." + std::to_string(round) + ".json" : advice_base;
        std::ofstream(saved) << advice::advice_to_json(c.advice).dump(2) << "\n";
        if (total_rounds > 1) std::ofstream(advice_base) << advice::advice_to_json(c.advice).dump(2) << "\n";
        rj["advice_file"] = saved;
        ap.set.source = saved;
        log("advice saved to " + saved);
      }
      announce_advice(ap, "advisor");
      rj["hints"] = ap.report["hints"];
      route::RouterOptions o2 = opt;
      o2.advice = ap.set;
      o2.sink = nullptr;
      route_t0 = std::chrono::steady_clock::now();
      Run next = run_once(o2, round, false);
      for (int p = 0; p < route::kWorkPhases; ++p) all_work.phase[static_cast<std::size_t>(p)] += next.work.phase[static_cast<std::size_t>(p)];
      const bool adopt = route::route_result_better(next.res, res);
      rj["routed_after"] = next.res.routed;
      rj["unadvised_best_routed"] = next.unadvised_best;
      rj["advised_best_routed"] = next.advised_best;
      rj["adopted"] = adopt;
      log(fmt("advisor round %d: %d -> %d routed%s", round, res.routed, next.res.routed, adopt ? " (the advised run's result is kept)" : " (no gain: the earlier result stands)"));
      for (auto& row : next.rows) variant_rows.push_back(std::move(row));
      previous = ap.report;
      previous["routed_after"] = next.res.routed;
      previous["routed_before"] = res.routed;
      if (adopt) {
        res = std::move(next.res);
        ran = std::move(next.ran);
        best_index = next.best_index;
        best_name = std::move(next.best_name);
      }
      advisor_rounds.push_back(rj);
      if (opt.sink) opt.sink->publish(server::stage("advisor", "end", fmt("%d routed", res.routed)));
      if (res.routed >= res.connections) break;
    }
  }
  if (!advisor_rounds.empty()) {
    if (advice_summary.is_null()) advice_summary = nlohmann::json::object();
    advice_summary["advisor"] = {{"program", job.advisor.substr(0, job.advisor.find_first_of(" \t"))}, {"rounds", advisor_rounds}};
  }
  if (kb) {
    kb->record_run(feat, name, unadvised_only(ran), best_index >= route::kAdvisedIndex ? -1 : best_index, res.routed, res.connections, res.seconds);
    std::vector<learn::FailedConnection> failed;
    for (const auto& u : res.unrouted) failed.push_back({u.net, u.a, u.b, 1});
    kb->record_failures(feat.hash, failed);
  }
  // Soft zones (doc 05 §36): the router cut through stale fills and trusted them for connectivity. Refill them for
  // the new copper as KiCad will, count what is still unconnected, and route those connections again with the
  // refilled fills as fixed copper (pass 2, its own budget equal to the first pass's).
  nlohmann::json refill_summary;
  int unconnected_after_refill = -1;
  if (opt.soft_zones) {
    model::Board routed = *route_board;
    auto add_copper = [](model::Board& b, const route::RouteResult& r) {
      b.tracks.insert(b.tracks.end(), r.tracks.begin(), r.tracks.end());
      b.vias.insert(b.vias.end(), r.vias.begin(), r.vias.end());
    };
    auto unconnected = [&](const model::Board& b) {
      drc::DrcOptions d;
      d.connectivity_only = true;
      d.dangling = false;
      return static_cast<int>(drc::run_drc(b, rules, d).unconnected.size());
    };
    add_copper(routed, res);
    auto rf = drc::refill_zones(routed, rules);
    model::Board filled_now = rf.board;  // the refilled board for the copper res holds (judged for thermal reliefs below)
    for (const auto& w : rf.warnings) log("warning: " + w);
    const int before = unconnected(rf.board);
    log(fmt("soft zones: after refilling %d zones, %d items unconnected", rf.zones, before));
    refill_summary = {{"zones", rf.zones}, {"unconnected", before}, {"warnings", rf.warnings}};
    unconnected_after_refill = before;
    // The repair has the first pass's work budget but only what is left of the time limit (rule 5).
    const double time_left = opt.time_limit_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - route_t0).count();
    if (before > 0 && job.refill_repair && time_left <= 0) log("refill repair: skipped, the time limit is used up");
    if (before > 0 && job.refill_repair && time_left > 0) {
      route::RouterOptions o2 = opt;
      o2.soft_zones = false;
      o2.sink = nullptr;
      o2.time_limit_s = time_left;
      const auto r2 = route::Router(rf.board, rules, o2).run();
      log(fmt("refill repair: routed %d/%d connections, %zu tracks, %zu vias, %ld expansions", r2.routed, r2.connections, r2.tracks.size(),
              r2.vias.size(), r2.expansions));
      // The repair's copper changes the fills once more. It is kept only if the count after that refill is lower
      // (rule 4); otherwise the first pass stands.
      add_copper(routed, r2);
      model::Board refilled_again = drc::refill_zones(routed, rules).board;
      const int after = unconnected(refilled_again);
      const bool kept = after < before;
      log(fmt("refill repair: %d items unconnected after refilling again%s", after, kept ? "" : " (no better: repair discarded)"));
      if (kept) {
        res.tracks.insert(res.tracks.end(), r2.tracks.begin(), r2.tracks.end());
        res.vias.insert(res.vias.end(), r2.vias.begin(), r2.vias.end());
        unconnected_after_refill = after;
        filled_now = std::move(refilled_again);
      }
      refill_summary["repair"] = {{"connections", r2.connections}, {"routed", r2.routed}, {"tracks", r2.tracks.size()},
                                  {"vias", r2.vias.size()}, {"expansions", r2.expansions}, {"unconnected", after}, {"kept", kept}};
    }
    // Thermal reliefs (KiCad's starved_thermal). New copper of other nets can cut a pad's spokes off the pour, or leave them
    // in an island only that pad reaches: the soak of 2026-10-25 found six on interf_u, which the unconnected count above
    // cannot see. They are named in the log and the summary, and repaired pad by pad where new copper crosses the spokes.
    std::set<std::pair<int, int>> starved_before;  // (pad, layer) the input board already has
    bool before_known = false;
    auto starved_new = [&](const model::Board& filled) {
      std::vector<drc::StarvedThermal> found = drc::starved_thermals(filled, rules), fresh;
      if (!found.empty() && !before_known) {
        for (const auto& s : drc::starved_thermals(drc::refill_zones(*route_board, rules).board, rules)) starved_before.insert({s.pad, s.layer});
        before_known = true;
      }
      for (const auto& s : found)
        if (!starved_before.count({s.pad, s.layer})) fresh.push_back(s);
      return fresh;
    };
    const auto starved = starved_new(filled_now);
    int starved_after = static_cast<int>(starved.size());
    if (!starved.empty()) {
      std::string pads;
      for (const auto& s : starved) {
        const auto& pad = route_board->pads[static_cast<std::size_t>(s.pad)];
        pads += fmt("%s%s.%s", pads.empty() ? "" : ", ", pad.footprint >= 0 ? route_board->footprints[static_cast<std::size_t>(pad.footprint)].reference.c_str() : "",
                    pad.number.c_str());
      }
      log(fmt("thermal reliefs: %zu starved by the new copper (%s)", starved.size(), pads.c_str()));
      const double thermal_time = opt.time_limit_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - route_t0).count();
      if (job.refill_repair && thermal_time <= 0) log("thermal repair: skipped, the time limit is used up");
      if (job.refill_repair && thermal_time > 0) {
        const Coord clearance = std::max(rules.minimums.clearance, rules.classes.empty() ? Coord{0} : rules.default_class().clearance);
        auto touches = [](const std::vector<model::Zone>& ks, const geom::Shape& sh, model::LayerMask layers) {
          return std::any_of(ks.begin(), ks.end(), [&](const model::Zone& k) {
            return (k.copper & layers) && geom::closer_than(sh, geom::Shape::polygon(k.outline.front(), 0), 1);
          });
        };
        auto via_layers = [](const model::Via& v) { return ((model::LayerMask{2} << v.layer_bottom) - 1) & ~(model::layer_bit(v.layer_top) - 1); };
        auto nets_into = [&](const std::vector<model::Zone>& ks) {
          std::set<model::NetId> n;
          for (const auto& t : res.tracks)
            if (touches(ks, geom::Shape::segment(t.a, t.b, t.width / 2), model::layer_bit(t.layer))) n.insert(t.net);
          for (const auto& v : res.vias)
            if (touches(ks, geom::Shape::point(v.pos, v.size / 2), via_layers(v))) n.insert(v.net);
          return n;
        };
        // Keep-outs per starved pad: as many of its spokes as the board asks for (those the fewest new nets cross), each with a
        // minimum-width band of pour beside it, stopping short of other nets' pads so those stay reachable on this layer.
        std::vector<std::vector<model::Zone>> lanes_of(starved.size());
        std::vector<model::Zone> keepouts;
        for (std::size_t i = 0; i < starved.size(); ++i) {
          const auto& s = starved[i];
          const auto& pad = route_board->pads[static_cast<std::size_t>(s.pad)];
          const auto& zone = route_board->zones[static_cast<std::size_t>(s.zone)];
          const geom::Box near = geom::Box{pad.pos.x, pad.pos.y, pad.pos.x, pad.pos.y}.inflated(10'000'000);
          geom::Rings others;
          for (const auto& p : route_board->pads)
            if (p.net != pad.net && (p.copper & model::layer_bit(s.layer)))
              for (const auto& sh : drc::pad_shapes(p))
                if (sh.box.intersects(near))
                  for (auto& r : geom::shape_rings(sh, clearance, 5'000)) others.push_back(std::move(r));
          std::vector<std::pair<std::size_t, std::vector<model::Zone>>> spoke_lanes;  // (nets crossing it, keep-outs)
          for (auto& ring : drc::thermal_spokes(pad, zone)) {
            geom::Rings lane = geom::offset(geom::Rings{std::move(ring)}, zone.min_thickness + clearance, geom::Corners::Chamfer, 5'000);
            if (!others.empty()) lane = geom::subtract(lane, others);
            std::vector<model::Zone> ks;
            for (auto& poly : geom::polygons(lane)) {
              model::Zone k;
              k.rule_area = k.keepout_tracks = k.keepout_vias = true;
              k.copper = model::layer_bit(s.layer);
              k.outline = {std::move(poly.outer)};
              ks.push_back(std::move(k));
            }
            spoke_lanes.push_back({nets_into(ks).size(), std::move(ks)});
          }
          std::stable_sort(spoke_lanes.begin(), spoke_lanes.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
          const std::size_t need = spoke_lanes.size();
          for (std::size_t k = 0; k < need; ++k)
            for (auto& z : spoke_lanes[k].second) {
              lanes_of[i].push_back(z);
              keepouts.push_back(std::move(z));
            }
        }
        // One transaction per starved pad, in pad order: the nets whose new copper crosses its lanes lose their new copper
        // and are routed again (alone: the other nets are skipped) with every starved pad's lanes kept out. Kept only if
        // fewer reliefs are starved and nothing more is unconnected after a refill (rule 4). Ripping all such nets at once
        // cured interf_u's seven reliefs but lost seven connections.
        int u_cur = unconnected_after_refill;
        int s_cur = static_cast<int>(starved.size());
        int tried = 0, kept_pads = 0;
        std::size_t ripped = 0;
        for (std::size_t i = 0; i < starved.size() && s_cur > 0; ++i) {
          const double left = opt.time_limit_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - route_t0).count();
          if (left <= 0) {
            log("thermal repair: stopped, the time limit is used up");
            break;
          }
          const std::set<model::NetId> rip = nets_into(lanes_of[i]);
          if (rip.empty()) continue;  // spokes cut off further away: nothing of this pad's to move
          ++tried;
          route::RouteResult kept;
          for (const auto& t : res.tracks)
            if (!rip.count(t.net)) kept.tracks.push_back(t);
          for (const auto& v : res.vias)
            if (!rip.count(v.net)) kept.vias.push_back(v);
          model::Board b3 = *route_board;
          add_copper(b3, kept);
          model::Board b3f = drc::refill_zones(b3, rules).board;
          b3f.zones.insert(b3f.zones.end(), keepouts.begin(), keepouts.end());
          route::RouterOptions o3 = opt;
          o3.soft_zones = false;
          o3.sink = nullptr;
          o3.time_limit_s = left;
          for (const auto& n : route_board->nets)
            if (n.id > 0 && !rip.count(n.id)) o3.skip_nets.push_back(n.name);
          const auto r3 = route::Router(b3f, rules, o3).run();
          add_copper(b3, r3);
          const model::Board judged = drc::refill_zones(b3, rules).board;
          const int u3 = unconnected(judged), s3 = static_cast<int>(starved_new(judged).size());
          if (s3 < s_cur && u3 <= u_cur) {
            kept.tracks.insert(kept.tracks.end(), r3.tracks.begin(), r3.tracks.end());
            kept.vias.insert(kept.vias.end(), r3.vias.begin(), r3.vias.end());
            res.tracks = std::move(kept.tracks);
            res.vias = std::move(kept.vias);
            u_cur = u3;
            s_cur = s3;
            ++kept_pads;
            ripped += rip.size();
          }
        }
        log(fmt("thermal repair: %d of %d pads repaired (%zu nets routed again); %d starved and %d unconnected after refilling", kept_pads, tried, ripped,
                s_cur, u_cur));
        if (kept_pads > 0) unconnected_after_refill = u_cur;
        starved_after = s_cur;
        refill_summary["thermal_repair"] = {{"starved", starved.size()}, {"pads_tried", tried}, {"pads_repaired", kept_pads}, {"nets_rerouted", ripped},
                                            {"starved_after", s_cur}, {"unconnected", u_cur}};
      }
    }
    refill_summary["starved_thermals"] = starved_after;
    out.unconnected_after_refill = unconnected_after_refill;
  }
  // After-route passes (doc 05 §39). Each works on the routed copper as a transaction: its result is judged by
  // the number of unconnected items (after a refill when fills are soft or vias were added) and dropped if that
  // number went up.
  nlohmann::json widen_summary, stitch_summary;
  bool plane_zones = false;
  for (const auto& z : route_board->zones) plane_zones = plane_zones || (!z.rule_area && z.net > 0);
  const bool si_stitch = job.si_rules && job.si_stitch && !critical.empty() && plane_zones && route_board->copper_count() > 1;
  int si_missing_before = 0;
  {
    // Unconnected items per net (keyed by the report's text, which names the net), and all of them under "".
    using Unconnected = std::map<std::string, int>;
    auto judge = [&](const std::vector<model::Track>& tracks, const std::vector<model::Via>& vias, bool refill) {
      model::Board bd = *route_board;
      bd.tracks.insert(bd.tracks.end(), tracks.begin(), tracks.end());
      bd.vias.insert(bd.vias.end(), vias.begin(), vias.end());
      drc::DrcOptions d;
      d.connectivity_only = true;
      d.dangling = false;
      Unconnected n;
      n[""] = 0;
      if (refill) bd = drc::refill_zones(bd, rules).board;
      for (const auto& v : drc::run_drc(bd, rules, d).unconnected) ++n[v.description], ++n[""];
      // A wider track or a stitching via must not starve a thermal relief either (KiCad's starved_thermal).
      if (refill) n["starved thermal reliefs"] = static_cast<int>(drc::starved_thermals(bd, rules).size());
      return n;
    };
    // No net may have more unconnected items with a pass's result than without it. The total alone would let a
    // connection the pass made in one net (a stitching via that ties a loose piece of its plane) hide one it broke
    // in another.
    auto no_worse = [](const Unconnected& without, const Unconnected& with) {
      for (const auto& [net, n] : with) {
        const auto it = without.find(net);
        if (n > (it == without.end() ? 0 : it->second)) return false;
      }
      return true;
    };
    auto mm = [](Coord v) { return fmt("%.3f", nm_to_mm(v)); };
    if (job.widen_power) {
      if (opt.sink) opt.sink->publish(server::stage("widen", "begin", "power track widening"));
      passes::WidenOptions wo;
      wo.targets = widen_targets;
      wo.soft_zones = opt.soft_zones;
      wo.work_limit = job.pass_work;
      // Coupled pairs keep their pair width.
      {
        const drc::RuleEngine re(lb.board, rules);
        auto pairs = opt.diff_pairs ? route::named_pairs(lb.board, re) : std::vector<std::pair<model::NetId, model::NetId>>{};
        pairs.insert(pairs.end(), opt.pair_nets.begin(), opt.pair_nets.end());
        for (const auto& [a, b2] : pairs) wo.skip_nets.push_back(a), wo.skip_nets.push_back(b2);
      }
      auto widened = res.tracks;
      const auto wr = passes::widen_tracks(*route_board, rules, widened, res.vias, wo);
      bool kept = wr.widened > 0;
      int before = -1, after = -1;
      if (kept) {
        const auto without = judge(res.tracks, res.vias, opt.soft_zones), with = judge(widened, res.vias, opt.soft_zones);
        before = without.at("");
        after = with.at("");
        kept = no_worse(without, with);
      }
      nlohmann::json nets = nlohmann::json::array();
      Coord total = 0;
      for (const auto& n : wr.nets) {
        auto widths = [&](const std::map<Coord, Coord>& m) {
          nlohmann::json j = nlohmann::json::object();
          for (const auto& [w, len] : m) j[mm(w)] = nm_to_mm(len);
          return j;
        };
        auto text = [&](const std::map<Coord, Coord>& m) {
          std::string t;
          for (const auto& [w, len] : m) t += (t.empty() ? "" : ", ") + fmt("%.2f mm at %s", nm_to_mm(len), mm(w).c_str());
          return t;
        };
        const std::string& net_name = lb.board.nets[static_cast<std::size_t>(n.net)].name;
        total += n.widened_length;
        nets.push_back({{"net", net_name}, {"target_mm", nm_to_mm(n.target)}, {"source", n.source}, {"tracks_below_target", n.tracks},
                        {"tracks_widened", n.widened}, {"tracks_at_target", n.at_target}, {"widened_mm", nm_to_mm(n.widened_length)},
                        {"before_mm_by_width", widths(n.before)}, {"after_mm_by_width", widths(kept ? n.after : n.before)}});
        if (n.widened > 0)
          log(fmt("  widen %s (target %s mm, %s): %d of %d tracks, %.2f mm; before %s; after %s", net_name.c_str(), mm(n.target).c_str(), n.source.c_str(),
                  n.widened, n.tracks, nm_to_mm(n.widened_length), text(n.before).c_str(), text(n.after).c_str()));
      }
      log(fmt("widen power: %d tracks of %zu nets widened, %.2f mm, %ld checks%s%s", wr.widened, wr.nets.size(), nm_to_mm(total), wr.checks,
              wr.out_of_work ? " (work budget used up)" : "",
              wr.widened > 0 && !kept ? fmt("; discarded: a net has more unconnected items with it (%d in all, %d without)", after, before).c_str() : ""));
      widen_summary = {{"tracks_widened", kept ? wr.widened : 0}, {"widened_mm", kept ? nm_to_mm(total) : 0.0}, {"checks", wr.checks},
                       {"out_of_work", wr.out_of_work}, {"kept", kept}, {"nets", nets}};
      if (kept) {
        res.tracks = std::move(widened);
        if (unconnected_after_refill >= 0) out.unconnected_after_refill = unconnected_after_refill = after;
      }
      if (opt.sink) opt.sink->publish(server::stage("widen", "end", fmt("%d tracks widened", kept ? wr.widened : 0)));
    }
    // Tuning profiles: each track of a single-ended profiled net narrowed to its profile's width on its layer (see above). Judged like the
    // other passes, though a narrower track keeps its ends, so it should never lose a connection.
    if (!tuning_widths.empty()) {
      auto narrowed = res.tracks;
      int n_narrowed = 0;
      std::map<std::string, int> by_class;
      for (auto& t : narrowed) {
        const auto it = tuning_widths.find(t.net);
        if (it == tuning_widths.end() || t.layer < 0 || static_cast<std::size_t>(t.layer) >= it->second.size()) continue;
        const Coord w = it->second[static_cast<std::size_t>(t.layer)];
        if (w > 0 && t.width > w) t.width = w, ++n_narrowed, ++by_class[rules.class_for(lb.board.nets[static_cast<std::size_t>(t.net)].name).name];
      }
      bool kept = n_narrowed > 0;
      if (kept) kept = no_worse(judge(res.tracks, res.vias, opt.soft_zones), judge(narrowed, res.vias, opt.soft_zones));
      log(fmt("tuning profiles: %d track%s narrowed to the profile's width on their layer%s", n_narrowed, n_narrowed == 1 ? "" : "s",
              n_narrowed > 0 && !kept ? "; discarded: a net has more unconnected items with it" : ""));
      for (auto& e : tuning_summary) e["tracks_narrowed"] = kept ? by_class[e["net_class"].get<std::string>()] : 0;
      if (kept) res.tracks = std::move(narrowed);
    }
    // --si-rules closes the audit loop (doc 18 B3, doc 05 §57): a layer change of a critical net that the reference-return audit finds with
    // the same reference net on both sides and no via of that net within the audit's radius asks for return-path stitching of that net
    // to that plane. A change between two different reference nets closes through a capacitor, which no pass places; it stays a warning.
    auto returns = stitch_returns;
    if (si_stitch) {
      model::Board routed = *route_board;
      routed.tracks.insert(routed.tracks.end(), res.tracks.begin(), res.tracks.end());
      routed.vias.insert(routed.vias.end(), res.vias.begin(), res.vias.end());
      analysis::ReturnPathOptions ro;
      for (const auto& c : critical) ro.name_patterns.push_back(lb.board.nets[static_cast<std::size_t>(c.net)].name);
      ro.refill_always = true;
      const Coord radius = static_cast<Coord>(ro.stitch_radius_mm * 1e6);
      for (const auto& n : analysis::analyse_return_paths(routed, rules, ro).nets) {
        si_missing_before += n.missing_stitches;
        for (const auto& ch : n.changes) {
          if (!ch.missing || ch.ref_from.empty() || ch.ref_from != ch.ref_to) continue;
          const model::NetId plane = lb.board.net_by_name(ch.ref_from);
          const bool seen = std::any_of(returns.begin(), returns.end(), [&](const auto& y) { return y.signal == n.net && y.plane == plane; });
          if (plane > 0 && plane != n.net && !seen) returns.push_back({n.net, plane, std::min(radius, static_cast<Coord>(crules::builtin_catalogue().stitch_via_within_mm * 1e6)), "si-rules"});
        }
      }
      log(fmt("si-rules: %d layer change%s of critical nets without a return stitch; %zu net%s to stitch to their plane", si_missing_before,
              si_missing_before == 1 ? "" : "s", returns.size() - stitch_returns.size(), returns.size() - stitch_returns.size() == 1 ? "" : "s"));
    }
    if (stitch_planes || !returns.empty() || stitch_return) {
      if (opt.sink) opt.sink->publish(server::stage("stitch", "begin", "stitching vias"));
      passes::StitchOptions so;
      so.planes = stitch_planes;
      so.pitch = static_cast<Coord>((job.stitch_pitch_mm > 0 ? job.stitch_pitch_mm : crules::builtin_catalogue().stitch_via_within_mm) * 1e6);
      so.returns = returns;
      so.work_limit = job.pass_work;
      model::Board bd = *route_board;
      bd.tracks.insert(bd.tracks.end(), res.tracks.begin(), res.tracks.end());
      bd.vias.insert(bd.vias.end(), res.vias.begin(), res.vias.end());
      const auto sr = passes::stitch_vias(bd, rules, so);
      for (const auto& w : sr.warnings) log("warning: " + w);
      bool kept = !sr.vias.empty();
      int before = -1, after = -1;
      if (kept) {
        auto with = res.vias;
        with.insert(with.end(), sr.vias.begin(), sr.vias.end());
        const auto without = judge(res.tracks, res.vias, true), with_vias = judge(res.tracks, with, true);
        before = without.at("");
        after = with_vias.at("");
        kept = no_worse(without, with_vias);
      }
      std::map<model::NetId, std::pair<int, int>> per_net;  // net -> (grid vias, return-path vias)
      for (std::size_t i = 0; i < sr.vias.size() && kept; ++i) ++(sr.return_path[i] ? per_net[sr.vias[i].net].second : per_net[sr.vias[i].net].first);
      nlohmann::json nets = nlohmann::json::array(), rets = nlohmann::json::array();
      int grid = 0, ret = 0;
      for (const auto& [n, c] : per_net) {
        const std::string& net_name = lb.board.nets[static_cast<std::size_t>(n)].name;
        nets.push_back({{"net", net_name}, {"plane", c.first}, {"return_path", c.second}});
        log(fmt("  stitch %s: %d plane vias, %d return-path vias", net_name.c_str(), c.first, c.second));
        grid += c.first;
        ret += c.second;
      }
      for (const auto& r : sr.returns) {
        const std::string& sig = lb.board.nets[static_cast<std::size_t>(r.signal)].name;
        const std::string& pl = lb.board.nets[static_cast<std::size_t>(r.plane)].name;
        const int now = kept ? r.stitched_after : r.stitched_before;
        rets.push_back({{"net", sig}, {"plane_net", pl}, {"source", r.source}, {"distance_mm", nm_to_mm(r.distance)}, {"signal_vias", r.signal_vias},
                        {"stitched_before", r.stitched_before}, {"stitched_after", now}, {"unstitched_before", r.signal_vias - r.stitched_before},
                        {"unstitched_after", r.signal_vias - now}});
        log(fmt("  return path %s over %s (%s, within %s mm): %d vias, %d stitched before, %d after", sig.c_str(), pl.c_str(), r.source.c_str(),
                mm(r.distance).c_str(), r.signal_vias, r.stitched_before, now));
      }
      if (stitch_return && sr.returns.empty()) log("  stitching: no net needs a reference plane (component rules of kind reference_plane, or --stitch-return)");
      log(fmt("stitch vias: %d plane, %d return-path; %d grid sites in overlapping fills (%d already tied, %d blocked), %d dropped after the refill, "
              "%ld checks%s%s",
              grid, ret, sr.sites, sr.site_served, sr.site_blocked, sr.dropped, sr.checks, sr.out_of_work ? " (work budget used up)" : "",
              sr.discarded ? "; withdrawn: the refill with the vias cut a fill"
              : !sr.vias.empty() && !kept ? fmt("; discarded: a net has more unconnected items with them (%d in all, %d without)", after, before).c_str() : ""));
      stitch_summary = {{"plane_vias", grid}, {"return_path_vias", ret}, {"sites", sr.sites}, {"sites_already_tied", sr.site_served},
                        {"sites_blocked", sr.site_blocked}, {"dropped", sr.dropped}, {"checks", sr.checks}, {"out_of_work", sr.out_of_work},
                        {"kept", kept}, {"pitch_mm", nm_to_mm(so.pitch)}, {"nets", nets}, {"return_paths", rets}};
      if (kept) {
        for (std::size_t i = 0; i < sr.vias.size(); ++i) {
          // Ids above the router's own, so the viewer shows the vias without replacing anything.
          if (opt.sink) opt.sink->publish(server::via_add(static_cast<server::ObjectId>(1'000'000'000 + i), sr.vias[i]));
          res.vias.push_back(sr.vias[i]);
        }
        if (unconnected_after_refill >= 0) out.unconnected_after_refill = unconnected_after_refill = after;
      }
      if (opt.sink) opt.sink->publish(server::stage("stitch", "end", fmt("%d vias added", grid + ret)));
    }
  }
  nlohmann::json pour_summary;
  if (!pours.empty()) {
    // The pours' fills for the copper as written: the engine's refill (doc 05 §36), as KiCad's would be after pressing B.
    model::Board routed = lb.board;
    routed.tracks.insert(routed.tracks.end(), res.tracks.begin(), res.tracks.end());
    routed.vias.insert(routed.vias.end(), res.vias.begin(), res.vias.end());
    const auto filled = drc::refill_zones(routed, rules);
    pour_summary = nlohmann::json::array();
    for (auto& [i, p] : pours) {
      p.zone.fills = filled.board.zones[i].fills;
      double area = 0;
      for (const auto& [l, ring] : p.zone.fills) area += std::abs(geom::area(ring));
      const int layer = std::countr_zero(p.zone.copper);
      log(fmt("pour %s on %s: %zu filled polygons, %.0f mm2 after routing", lb.board.nets[static_cast<std::size_t>(p.zone.net)].name.c_str(),
              lb.board.copper_name(layer).c_str(), p.zone.fills.size(), area / 1e12));
      pour_summary.push_back({{"net", lb.board.nets[static_cast<std::size_t>(p.zone.net)].name}, {"layer", lb.board.copper_name(layer)},
                              {"reason", p.reason}, {"clearance_mm", nm_to_mm(p.zone.clearance)}, {"polygons", p.zone.fills.size()},
                              {"area_mm2", std::round(area / 1e10) / 100.0}});
    }
  }
  if (!job.out.empty()) {
    io::BoardEditor ed(lb, opt.seed);
    for (const auto& t : res.tracks) ed.add_track(t);
    for (const auto& v : res.vias) ed.add_via(v);
    for (const auto& [i, p] : pours) ed.set_zone_fills(i, p.zone.fills);
    ed.save(job.out);
    copy_project_files(job.in, job.out);
  }
  // Reference-return audit (issue #7, doc 05 §42): the selected nets on the board as routed, fills redone for the new copper.
  nlohmann::json return_path_summary;
  {
    const auto& rp = job.return_paths;
    if (!rp.name_patterns.empty() || !rp.net_classes.empty() || rp.diff_pairs) {
      auto ro = rp;
      ro.refill_always = true;  // the fills in the file predate the new vias and tracks
      if (ro.diff_pairs) {
        const drc::RuleEngine re(*route_board, rules);
        ro.pair_nets = route::named_pairs(*route_board, re);
      }
      model::Board routed = *route_board;
      routed.tracks.insert(routed.tracks.end(), res.tracks.begin(), res.tracks.end());
      routed.vias.insert(routed.vias.end(), res.vias.begin(), res.vias.end());
      const auto rpr = analysis::analyse_return_paths(routed, rules, ro);
      log(fmt("return paths: %zu net%s analysed", rpr.nets.size(), rpr.nets.size() == 1 ? "" : "s"));
      for (const auto& l : analysis::return_path_lines(routed, rpr)) log("  " + l);
      return_path_summary = analysis::return_path_json(routed, rpr);
      if (opt.sink)
        for (const auto& n : rpr.nets) opt.sink->publish(analysis::return_path_message(routed, n));
    }
  }
  // Critical nets after the route (issue #24): vias, length and the plane crossings of each, so the three rules can be judged on the board they produced.
  nlohmann::json critical_summary;
  if (!critical.empty()) {
    std::map<model::NetId, int> vias_by_net;
    std::map<model::NetId, double> length_by_net;
    for (const auto& v : res.vias) ++vias_by_net[v.net];
    for (const auto& t : res.tracks) length_by_net[t.net] += std::hypot(static_cast<double>(t.a.x - t.b.x), static_cast<double>(t.a.y - t.b.y)) / 1e6;
    std::map<std::string, analysis::NetReturn> audit;
    std::vector<std::string> audit_messages;  // each critical net's return path for the viewer's overlay
    bool has_zones = false;
    for (const auto& z : route_board->zones) has_zones = has_zones || (!z.rule_area && z.net > 0);
    if (has_zones && route_board->copper_count() > 1) {
      analysis::ReturnPathOptions ro;
      for (const auto& c : critical) ro.name_patterns.push_back(lb.board.nets[static_cast<std::size_t>(c.net)].name);
      ro.refill_always = true;  // the fills in the file predate the new vias and tracks
      model::Board routed = *route_board;
      routed.tracks.insert(routed.tracks.end(), res.tracks.begin(), res.tracks.end());
      routed.vias.insert(routed.vias.end(), res.vias.begin(), res.vias.end());
      for (auto& n : analysis::analyse_return_paths(routed, rules, ro).nets) {
        audit_messages.push_back(analysis::return_path_message(routed, n));
        audit[n.name] = std::move(n);
      }
    }
    critical_summary = nlohmann::json::object();
    critical_summary["options"] = {{"first", opt.critical_first}, {"via_mult", opt.critical_via_mult}, {"split_cost", opt.split_cost}, {"split_hard", opt.split_hard},
                                   {"via_max", job.critical_via_max}};
    critical_summary["nets"] = nlohmann::json::array();
    int total_vias = 0, total_crossings = 0, over_limit = 0, total_missing = 0;
    const double ro_radius_mm = analysis::ReturnPathOptions{}.stitch_radius_mm, ro_warn_detour_mm = analysis::ReturnPathOptions{}.warn_detour_mm;
    for (const auto& c : critical) {
      const std::string& net_name = lb.board.nets[static_cast<std::size_t>(c.net)].name;
      const int vias = vias_by_net.count(c.net) ? vias_by_net[c.net] : 0;
      nlohmann::json row{{"name", net_name}, {"tier", c.tier}, {"why", c.why}, {"vias", vias}, {"length_mm", std::round((length_by_net.count(c.net) ? length_by_net[c.net] : 0.0) * 1000) / 1000}};
      std::vector<std::string> warn;
      int unrouted = 0;  // failures are written "NET: A -> B (...)"
      for (const auto& f : res.failures)
        if (f.rfind(net_name + ": ", 0) == 0) ++unrouted;
      row["unrouted"] = unrouted;
      if (unrouted > 0) warn.push_back(fmt("%d connection%s not routed", unrouted, unrouted == 1 ? "" : "s"));
      if (job.critical_via_max > 0 && vias > job.critical_via_max) {
        warn.push_back(fmt("%d vias, over the limit of %d", vias, job.critical_via_max));
        ++over_limit;
      }
      if (const auto it = audit.find(net_name); it != audit.end()) {
        const auto& n = it->second;
        row["void_crossings"] = n.void_count;
        row["detour_mm"] = std::round(n.detour_mm * 1000) / 1000;
        row["splits"] = n.split_count;
        row["uncovered_mm"] = std::round(n.uncovered_mm * 1000) / 1000;
        row["missing_stitches"] = n.missing_stitches;
        total_crossings += n.void_count;
        total_missing += n.missing_stitches;
        if (n.split_count > 0) warn.push_back(fmt("crosses %d split%s of its reference plane", n.split_count, n.split_count == 1 ? "" : "s"));
        else if (n.void_count > 0) warn.push_back(fmt("runs over %d void%s of its reference plane (detour %.2f mm)", n.void_count, n.void_count == 1 ? "" : "s", n.detour_mm));
        // Where (doc 18 B3): each layer change without a return closure and each void whose detour is over the audit's threshold, with its place.
        int listed = 0, more = 0;
        auto place = [&](const std::string& w) { (++listed <= 8 ? warn.push_back(w) : void(++more)); };
        for (const auto& ch : n.changes)
          if (ch.missing)
            place(fmt("layer change %s to %s at (%.3f, %.3f) mm: %s", lb.board.copper_name(ch.from_layer).c_str(), lb.board.copper_name(ch.to_layer).c_str(),
                      nm_to_mm(ch.at.x), nm_to_mm(ch.at.y),
                      ch.ref_from.empty() || ch.ref_to.empty() ? "no reference copper on one side"
                      : ch.ref_from == ch.ref_to ? fmt("no via of %s within %.1f mm", ch.ref_from.c_str(), ro_radius_mm).c_str()
                                                 : fmt("references %s and %s, no capacitor between them within %.1f mm", ch.ref_from.c_str(), ch.ref_to.c_str(), ro_radius_mm).c_str()));
        for (const auto& v : n.voids)
          if (!v.no_path && v.detour_mm > ro_warn_detour_mm)
            place(fmt("detour of %.2f mm round a void on %s at (%.3f, %.3f) mm", v.detour_mm, lb.board.copper_name(v.layer).c_str(), nm_to_mm(v.entry.x), nm_to_mm(v.entry.y)));
        if (more > 0) warn.push_back(fmt("and %d more place%s", more, more == 1 ? "" : "s"));
      }
      row["warnings"] = warn;
      total_vias += vias;
      critical_summary["nets"].push_back(row);
    }
    critical_summary["totals"] = {{"nets", critical.size()}, {"vias", total_vias}, {"void_crossings", total_crossings}, {"over_via_limit", over_limit}};
    if (!audit.empty()) critical_summary["totals"]["missing_stitches"] = total_missing;
    if (si_stitch) critical_summary["totals"]["missing_stitches_before_stitching"] = si_missing_before;
    log(fmt("critical nets after the route: %zu nets, %d vias, %d plane void crossings%s%s", critical.size(), total_vias, total_crossings,
            audit.empty() ? " (no reference plane on the board: crossings not measured)" : fmt(", %d layer changes without a return stitch", total_missing).c_str(),
            si_stitch ? fmt(" (%d before the stitching pass)", si_missing_before).c_str() : ""));
    for (const auto& row : critical_summary["nets"])
      for (const auto& w : row["warnings"]) log("  " + row["name"].get<std::string>() + ": " + w.get<std::string>());
    if (opt.sink) {
      for (const auto& m : audit_messages) opt.sink->publish(m);
      opt.sink->publish(nlohmann::json{{"type", "critical_nets"}, {"nets", critical_summary["nets"]}, {"totals", critical_summary["totals"]}}.dump());
    }
  }
  // The whole job, every variant included: a portfolio on fewer threads than variants runs them one after
  // another, so the winner's own time understates the run several times over (by more than the variant count
  // when the winner is one of the quicker variants).
  const double job_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_job).count();
  log(fmt("routed %d/%d connections, %zu tracks, %zu vias, pitch %.3f mm, %ld expansions, %.2f s (best variant %.2f s)", res.routed,
          res.connections, res.tracks.size(), res.vias.size(), nm_to_mm(res.pitch), res.expansions, job_seconds, res.seconds));
  for (const auto& f : res.failures) log("  unrouted: " + f);
  {
    const auto t = all_work.total();
    log(fmt("work (all variants): %ld expansions, %ld flood cells, %ld cell checks, %ld fixed-copper checks, %ld field cells in %ld fields, "
            "%ld commits, %ld rips",
            t.expansions, t.flood_cells, t.cell_checks, t.fixed_checks, t.field_cells, t.fields, t.commits, t.rips));
  }
  for (std::size_t r = 0; r < res.escape_rings.size(); ++r)
    log(fmt("  deep-array ring %zu: %d of %d pins connected", r + 1, res.escape_rings[r].second, res.escape_rings[r].first));
  if (opt.cut_report) {
    auto line = [&](const route::CutLine& c) {
      return fmt("%s = %.2f mm: %d nets must cross, %d tracks fit", c.vertical ? "x" : "y", nm_to_mm(c.at), c.demand, c.capacity);
    };
    if (res.tightest_cut.demand > 0) log("  tightest cut line " + line(res.tightest_cut));
    for (const auto& c : res.over_cuts) log("  cut proof: unroutable across " + line(c));
  }
  out.items = items_json(lb.board, res);
  if (!job.items_out.empty()) std::ofstream(job.items_out) << out.items.dump();
  out.summary = {{"routed", res.routed},     {"connections", res.connections}, {"tracks", res.tracks.size()},
                 {"vias", res.vias.size()},  {"seconds", job_seconds},         {"variant_seconds", res.seconds},
                 {"expansions", res.expansions}, {"pitch_mm", nm_to_mm(res.pitch)}, {"failures", res.failures},
                 {"variant", best_index}, {"variant_name", best_name}, {"escape_corridors", res.escape_corridors},
                 {"escape_corridor_mm", res.escape_corridor_mm}, {"package_pin_edges", res.package_pin_edges}, {"escape_reclaimed", res.escape_reclaimed},
                 {"bundle_groups", res.bundle_groups}, {"bundle_spines", res.bundle_spines}, {"bundle_conns", res.bundle_conns}, {"bundle_lanes", res.bundle_lanes}, {"tunnels", res.tunnels}};
  if (!variant_rows.empty()) out.summary["variants"] = std::move(variant_rows);
  // M22 repair steps (D156), only when they ran: a run without them writes the summary it always did.
  for (const auto& [k, v] : {std::pair<const char*, long>{"via_shoves", res.via_shoves}, {"via_shove_trials", res.via_shove_trials}, {"window_escalations", res.window_escalations},
                             {"via_retried", res.via_retried}, {"fanout_holds", res.fanout_holds}})
    if (v > 0) out.summary[k] = v;
  if (!refill_summary.is_null()) {
    out.summary["refill"] = refill_summary;
    out.summary["unconnected_after_refill"] = unconnected_after_refill;
  }
  if (!return_path_summary.is_null()) out.summary["return_paths"] = return_path_summary;
  if (!critical_summary.is_null()) out.summary["critical_nets"] = critical_summary;
  if (!tuning_summary.is_null()) out.summary["tuning_profiles"] = tuning_summary;
  if (!footprint_summary.is_null()) out.summary["footprint_check"] = footprint_summary;
  if (!advice_summary.is_null()) out.summary["advice"] = advice_summary;
  if (!cr_classes.empty()) out.summary["component_rule_net_classes"] = cr_classes;
  if (!widen_summary.is_null()) out.summary["widen"] = widen_summary;
  if (!stitch_summary.is_null()) out.summary["stitch"] = stitch_summary;
  if (!pour_summary.is_null()) out.summary["pours"] = pour_summary;
  out.summary["copper_layers"] = lb.board.copper_count();
  // Style metric of the new copper (doc 05 §39.2): measured only, never fed back into routing.
  out.summary["style"] = passes::style_json(lb.board, passes::measure_style(lb.board, res.tracks, {}, res.vias));
  out.summary["work"] = work_json(res.work);
  out.summary["work_all_variants"] = work_json(all_work);
  // Differential pairs (doc 05 §15): how each wanted pair came out, measured on the new copper (only when pairs are on).
  if (opt.diff_pairs || !opt.pair_nets.empty()) {
    const drc::RuleEngine re(lb.board, rules);
    auto want = opt.diff_pairs ? route::named_pairs(lb.board, re) : std::vector<std::pair<model::NetId, model::NetId>>{};
    for (const auto& p : opt.pair_nets)
      if (std::find(want.begin(), want.end(), p) == want.end() && std::find(want.begin(), want.end(), std::make_pair(p.second, p.first)) == want.end())
        want.push_back(p);
    nlohmann::json pj = nlohmann::json::array();
    log(fmt("differential pairs: %zu wanted, %d coupled pair routes (re-coupling included), %ld work units", want.size(), res.pairs, res.pair_work));
    out.summary["pair_work"] = res.pair_work;
    for (const auto& [na, nb] : want) {
      const auto pr = route::pair_rule(lb.board, rules, re, na, nb);
      const auto st = route::measure_pair(res.tracks, res.vias, na, nb, route::coupled_threshold(pr));
      const std::string& a = lb.board.nets[static_cast<std::size_t>(na)].name;
      const std::string& b = lb.board.nets[static_cast<std::size_t>(nb)].name;
      log(fmt("  %s / %s: %.1f mm, coupled %.0f %%, gap %.3f mm (target %.3f, %s), skew %.3f mm, vias %d/%d", a.c_str(), b.c_str(),
              (st.length_a + st.length_b) / 2e6, 100 * st.coupled_share(), st.gap_median / 1e6, nm_to_mm(pr.gap), pr.source.c_str(), st.skew() / 1e6,
              st.vias_a, st.vias_b));
      pj.push_back({{"net_a", a}, {"net_b", b}, {"coupled_share", st.coupled_share()}, {"gap_mm", st.gap_median / 1e6}, {"target_gap_mm", nm_to_mm(pr.gap)},
                    {"skew_mm", st.skew() / 1e6}, {"length_mm", (st.length_a + st.length_b) / 2e6}});
    }
    out.summary["pairs"] = pj;
  }
  // Buses: every group measured on the board as written (existing copper and the new), not on the router's own
  // book-keeping; `tracemaker buses` gives the same numbers for the output file.
  if (!bus_groups.empty()) {
    model::Board routed = lb.board;
    routed.tracks.insert(routed.tracks.end(), res.tracks.begin(), res.tracks.end());
    routed.vias.insert(routed.vias.end(), res.vias.begin(), res.vias.end());
    const auto joined = route::nets_joined(routed, rules);
    nlohmann::json bj = nlohmann::json::array();
    log(fmt("buses: %d connections routed as lanes, %d left their bus", res.bus_lanes, res.bus_left));
    for (const auto& g : bus_groups) {
      std::string line;
      bj.push_back(bus_report(routed, rules, joined, g, &line));
      log("  " + line);
    }
    out.summary["buses"] = bj;
    out.summary["bus_lanes"] = res.bus_lanes;
    out.summary["bus_left"] = res.bus_left;
  }
  if (opt.cut_report) {
    auto cj = [](const route::CutLine& c) {
      return nlohmann::json{{"axis", c.vertical ? "x" : "y"}, {"at_mm", nm_to_mm(c.at)}, {"demand", c.demand}, {"capacity", c.capacity}};
    };
    nlohmann::json over = nlohmann::json::array();
    for (const auto& c : res.over_cuts) over.push_back(cj(c));
    out.summary["cuts"] = {{"tightest", cj(res.tightest_cut)}, {"unroutable", over}};
  }
  if (!res.escape_rings.empty()) {
    nlohmann::json rings = nlohmann::json::array();
    for (const auto& [pins, done] : res.escape_rings) rings.push_back({{"pins", pins}, {"connected", done}});
    out.summary["escape_rings"] = rings;
  }
  out.summary["time_limited"] = res.time_limited;
  if (res.time_limited) log(fmt("warning: the time limit (%.0f s) ended the search before the work budget did: the result depends on machine speed", opt.time_limit_s));
  out.summary["open"] = res.connections - res.routed;
  out.summary["completion_pct"] = res.connections > 0 ? std::round(10000.0 * res.routed / res.connections) / 100.0 : 100.0;
  {
    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    out.summary["run"] = {{"version", std::string(version())}, {"board", name},  {"input", job.in},          {"output", job.out},
                          {"date", date},                      {"seed", opt.seed}, {"work_budget", opt.work_budget}, {"time_limit_s", opt.time_limit_s},
                          {"variants", variants},              {"threads", threads}};
    if (!job.options_given.is_null()) out.summary["run"]["options"] = job.options_given;
  }
  if (!job.report_out.empty()) {
    // The run report (doc 18 D3): what the router read of the rules, why connections are open, and the DRC before and after.
    const auto t_report = std::chrono::steady_clock::now();
    out.summary["rules"] = report::rules_json(job.in, lb.board, rules, engine_warnings);
    if (res.routed < res.connections) {
      advice::StuckInput si;
      si.board = &lb.board;
      si.rules = &rules;
      si.result = &res;
      si.board_name = name;
      si.seconds = res.seconds;
      si.max_unrouted = 500;
      si.soft_zones = opt.soft_zones;
      si.vias_off_pads_below_mm = opt.vias_off_pads ? opt.vias_off_pads_below_mm : 0.0;
      auto stuck = advice::build_stuck_report(si).json;
      for (const char* k : {"task", "disclosure", "advice_schema", "tracemaker_stuck_report"}) stuck.erase(k);  // for an advisor, not a designer
      out.summary["stuck"] = std::move(stuck);
    }
    // DRC of the board the router started from and of the board as written (KiCad reads that one), zones refilled as KiCad's judge does.
    model::Board after;
    if (!job.out.empty()) {
      after = io::read_board_file(job.out).board;
    } else {
      after = lb.board;
      after.tracks.insert(after.tracks.end(), res.tracks.begin(), res.tracks.end());
      after.vias.insert(after.vias.end(), res.vias.begin(), res.vias.end());
    }
    bool zones = false;
    for (const auto& z : lb.board.zones) zones = zones || (!z.rule_area && z.net > 0);
    out.summary["drc"] = report::drc_json(lb.board, after, rules, zones);
    log(fmt("report: DRC before and after%s: %d routing errors added (%.1f s)", zones ? " (zones refilled)" : "",
            out.summary["drc"]["added_routing_errors"].get<int>(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t_report).count()));
    std::string json_path = job.json_out;
    if (json_path.empty()) json_path = job.report_out + ".json";
    std::ofstream(job.report_out) << report::render(out.summary, report::format_for_path(job.report_out),
                                                    std::filesystem::path(json_path).filename().string());
    if (job.json_out.empty()) std::ofstream(json_path) << out.summary.dump(1);
    log("report written to " + job.report_out + " (its numbers are the fields of " + json_path + ")");
  }
  if (!job.json_out.empty()) std::ofstream(job.json_out) << out.summary.dump(1);
  if (server && job.hold) {
    log("routing finished; viewer still serving at " + server->url() + " (Ctrl-C to quit)");
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  opt.sink = nullptr;
  return out;
}

RouteJobResult run_layer_advisor(RouteJob job, std::vector<int> counts) {
  auto log = [&](const std::string& line) {
    if (job.log) job.log(line);
  };
  std::sort(counts.begin(), counts.end());
  counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
  if (counts.empty()) throw std::invalid_argument("--try-layers: no layer count given");
  if (!job.report_out.empty())
    throw std::invalid_argument("--try-layers: --report is not produced for the counts tried; route the chosen count again with --copper-layers N --report FILE");
  const auto input = io::read_board_file(job.in);
  const std::string final_out = job.out, final_json = job.json_out, final_items = job.items_out;
  nlohmann::json tried = nlohmann::json::array();
  std::optional<RouteJobResult> best;
  int best_count = 0, best_open = 0;
  std::string best_tmp;
  for (const int c : counts) {
    {  // A count the board cannot take is reported, not routed.
      auto lb = input;
      std::string why;
      io::BoardEditor ed(lb, job.opt.seed);
      if (!ed.set_copper_layer_count(c, &why)) {
        log(fmt("layers: %d copper layers not tried: %s", c, why.c_str()));
        tried.push_back({{"copper_layers", c}, {"tried", false}, {"why", why}});
        continue;
      }
    }
    RouteJob j = job;
    j.copper_layers = c;
    j.out = final_out.empty() ? std::string() : final_out + fmt(".%d-layers.tmp", c);
    j.json_out.clear();
    j.items_out.clear();
    j.hold = false;
    j.log = [&, c](const std::string& line) { log(fmt("[%d layers] ", c) + line); };
    log(fmt("layers: routing with %d copper layers", c));
    RouteJobResult r = run_route_job(std::move(j));
    const int open = r.unconnected_after_refill >= 0 ? r.unconnected_after_refill : r.result.connections - r.result.routed;
    const bool complete = r.exit_code() == 0;
    log(fmt("layers: %d copper layers: %d/%d routed%s, %zu vias, %s", c, r.result.routed, r.result.connections,
            r.unconnected_after_refill >= 0 ? fmt(", %d unconnected after the refill", r.unconnected_after_refill).c_str() : "", r.result.vias.size(),
            complete ? "complete" : "not complete"));
    tried.push_back({{"copper_layers", c}, {"tried", true}, {"routed", r.result.routed}, {"connections", r.result.connections},
                     {"unconnected_after_refill", r.unconnected_after_refill}, {"vias", r.result.vias.size()}, {"complete", complete},
                     {"seconds", r.summary.value("seconds", 0.0)}, {"time_limited", r.summary.value("time_limited", false)}});
    const std::string tmp = final_out.empty() ? std::string() : final_out + fmt(".%d-layers.tmp", c);
    if (!best || open < best_open) {
      if (!best_tmp.empty()) std::filesystem::remove(best_tmp);
      best = std::move(r);
      best_count = c;
      best_open = open;
      best_tmp = tmp;
    } else if (!tmp.empty()) {
      std::filesystem::remove(tmp);
    }
    if (complete) break;  // fewest first: the first count that completes is the answer
  }
  if (!best) throw std::runtime_error("--try-layers: the board can take none of the layer counts given");
  if (!final_out.empty()) {
    std::filesystem::rename(best_tmp, final_out);
    copy_project_files(job.in, final_out);
  }
  const bool complete = best->exit_code() == 0;
  log(complete ? fmt("layers: %d copper layers is the fewest of those tried that completes; written to %s", best_count,
                     final_out.empty() ? "(no output)" : final_out.c_str())
               : fmt("layers: no count tried completes; %d copper layers left the fewest open (%d)%s", best_count, best_open,
                     final_out.empty() ? "" : ("; written to " + final_out).c_str()));
  best->summary["layers"] = {{"tried", tried}, {"chosen", best_count}, {"complete", complete}};
  best->summary["run"]["output"] = final_out;
  best->summary["run"]["input"] = job.in;
  if (!final_items.empty()) std::ofstream(final_items) << best->items.dump();
  if (!final_json.empty()) std::ofstream(final_json) << best->summary.dump(1);
  return std::move(*best);
}

}  // namespace tmk::app
