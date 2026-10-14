// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/route_job.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "advice/advice.hpp"
#include "analysis/return_path.hpp"
#include "advice/advisor.hpp"
#include "advice/stuck_report.hpp"
#include "crules/engine.hpp"
#include "crules/names.hpp"
#include "drc/copper.hpp"
#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "drc/rule_engine.hpp"
#include "route/diff_pair.hpp"
#include "route/layer_limits.hpp"
#include "gpu/device.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "learn/knowledge_base.hpp"
#include "passes/stitch.hpp"
#include "passes/style.hpp"
#include "passes/widen.hpp"
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
  {
    const drc::RuleEngine re(*route_board, rules);
    for (const auto& w : re.warnings()) log("warning: " + w);
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
    if (variants > 1 || o.advice.active()) {
      std::vector<int> pick;
      if (use_kb && kb && variants < route::portfolio_size()) pick = kb->choose_variants(feat, route::portfolio_size(), variants, o.seed);
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
      const int after = unconnected(drc::refill_zones(routed, rules).board);
      const bool kept = after < before;
      log(fmt("refill repair: %d items unconnected after refilling again%s", after, kept ? "" : " (no better: repair discarded)"));
      if (kept) {
        res.tracks.insert(res.tracks.end(), r2.tracks.begin(), r2.tracks.end());
        res.vias.insert(res.vias.end(), r2.vias.begin(), r2.vias.end());
        unconnected_after_refill = after;
      }
      refill_summary["repair"] = {{"connections", r2.connections}, {"routed", r2.routed}, {"tracks", r2.tracks.size()},
                                  {"vias", r2.vias.size()}, {"expansions", r2.expansions}, {"unconnected", after}, {"kept", kept}};
    }
    out.unconnected_after_refill = unconnected_after_refill;
  }
  // After-route passes (doc 05 §39). Each works on the routed copper as a transaction: its result is judged by
  // the number of unconnected items (after a refill when fills are soft or vias were added) and dropped if that
  // number went up.
  nlohmann::json widen_summary, stitch_summary;
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
      for (const auto& v : drc::run_drc(refill ? drc::refill_zones(bd, rules).board : bd, rules, d).unconnected) ++n[v.description], ++n[""];
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
    if (stitch_planes || stitch_return) {
      if (opt.sink) opt.sink->publish(server::stage("stitch", "begin", "stitching vias"));
      passes::StitchOptions so;
      so.planes = stitch_planes;
      so.pitch = static_cast<Coord>((job.stitch_pitch_mm > 0 ? job.stitch_pitch_mm : crules::builtin_catalogue().stitch_via_within_mm) * 1e6);
      so.returns = stitch_returns;
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
  if (!job.out.empty()) {
    io::BoardEditor ed(lb, opt.seed);
    for (const auto& t : res.tracks) ed.add_track(t);
    for (const auto& v : res.vias) ed.add_via(v);
    ed.save(job.out);
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
                 {"variant", best_index}, {"variant_name", best_name}, {"escape_corridors", res.escape_corridors}};
  if (!variant_rows.empty()) out.summary["variants"] = std::move(variant_rows);
  if (!refill_summary.is_null()) {
    out.summary["refill"] = refill_summary;
    out.summary["unconnected_after_refill"] = unconnected_after_refill;
  }
  if (!return_path_summary.is_null()) out.summary["return_paths"] = return_path_summary;
  if (!footprint_summary.is_null()) out.summary["footprint_check"] = footprint_summary;
  if (!advice_summary.is_null()) out.summary["advice"] = advice_summary;
  if (!cr_classes.empty()) out.summary["component_rule_net_classes"] = cr_classes;
  if (!widen_summary.is_null()) out.summary["widen"] = widen_summary;
  if (!stitch_summary.is_null()) out.summary["stitch"] = stitch_summary;
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
  if (!job.json_out.empty()) std::ofstream(job.json_out) << out.summary.dump(1);
  if (server && job.hold) {
    log("routing finished; viewer still serving at " + server->url() + " (Ctrl-C to quit)");
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  opt.sink = nullptr;
  return out;
}

}  // namespace tmk::app
