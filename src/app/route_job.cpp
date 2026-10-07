// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/route_job.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "crules/engine.hpp"
#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "route/diff_pair.hpp"
#include "gpu/device.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "learn/knowledge_base.hpp"
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

}  // namespace

int default_gpu_device(bool use_gpu) {
  if (!use_gpu) return -1;
  const auto devs = gpu::list_devices();
  return devs.empty() ? -1 : devs.front().cuda_index;
}

RouteJobResult run_route_job(RouteJob job) {
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
  auto rules = io::read_design_rules(job.in);  // component rules may add net classes (in memory only)
  if (opt.via_in_pad && (rules.minimums.via_diameter <= 0 || rules.minimums.through_hole_diameter <= 0))
    log("warning: --via-in-pad: the board's minimum via diameter or drill could not be read, so no via in pad is placed");
  const std::string name = std::filesystem::path(job.in).filename().string();
  RouteJobResult out;
  std::unique_ptr<server::ViewerServer> server;
  if (job.view) {
    server::ServerOptions so;
    so.host = job.view_host;
    so.port = static_cast<std::uint16_t>(job.view_port);
    server = std::make_unique<server::ViewerServer>(so);
    server->publish(server::board_snapshot_json(lb.board, name));
    out.viewer_url = server->url();
    log("live view: " + out.viewer_url);
    opt.sink = server.get();
  }
  std::unique_ptr<FileSink> recorder;
  if (!job.record.empty() && !job.view) {
    recorder = std::make_unique<FileSink>(job.record);
    recorder->publish(server::board_snapshot_json(lb.board, name));
    opt.sink = recorder.get();
    opt.buffer_events = true;
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
  auto& res = out.result;
  std::vector<int> ran;
  int best_index = 0;
  std::string best_name;
  // The variant set is a setting, never derived from the thread count when a work budget makes the run
  // deterministic: then `--threads` only changes how fast the same variants finish (requirement N3, D47).
  const int threads = std::max(1, job.threads);
  const int variants = std::clamp(job.variants > 0 ? job.variants : opt.work_budget > 0 ? route::portfolio_size() : threads, 1,
                                  route::portfolio_size());
  if (variants > 1) {
    std::vector<int> pick;
    if (kb && variants < route::portfolio_size()) pick = kb->choose_variants(feat, route::portfolio_size(), variants, opt.seed);
    log(fmt("portfolio: %d variants on %d thread%s", variants, std::min(threads, variants), std::min(threads, variants) == 1 ? "" : "s"));
    auto pr = route::route_portfolio(*route_board, rules, opt, variants, pick, threads);
    for (std::size_t i = 0; i < pr.variants.size(); ++i)
      log(fmt("  variant %d %-30s routed %d%s", pr.indices[i], pr.variants[i].c_str(), pr.routed[i], static_cast<int>(i) == pr.best_variant ? "  <- best" : ""));
    ran = pr.indices;
    best_index = pr.indices[static_cast<std::size_t>(pr.best_variant)];
    best_name = pr.variants[static_cast<std::size_t>(pr.best_variant)];
    res = std::move(pr.best);
  } else {
    res = route::Router(*route_board, rules, opt).run();
    ran = {0};
  }
  if (kb) {
    kb->record_run(feat, name, ran, best_index, res.routed, res.connections, res.seconds);
    std::vector<learn::FailedConnection> failed;
    for (const auto& u : res.unrouted) failed.push_back({u.net, u.a, u.b, 1});
    kb->record_failures(feat.hash, failed);
  }
  if (!job.out.empty()) {
    io::BoardEditor ed(lb, opt.seed);
    for (const auto& t : res.tracks) ed.add_track(t);
    for (const auto& v : res.vias) ed.add_via(v);
    ed.save(job.out);
  }
  log(fmt("routed %d/%d connections, %zu tracks, %zu vias, pitch %.3f mm, %ld expansions, %.2f s", res.routed, res.connections, res.tracks.size(),
          res.vias.size(), nm_to_mm(res.pitch), res.expansions, res.seconds));
  for (const auto& f : res.failures) log("  unrouted: " + f);
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
                 {"vias", res.vias.size()},  {"seconds", res.seconds},         {"expansions", res.expansions},
                 {"pitch_mm", nm_to_mm(res.pitch)}, {"failures", res.failures}, {"variant", best_index}, {"variant_name", best_name},
                 {"escape_corridors", res.escape_corridors}};
  if (!cr_classes.empty()) out.summary["component_rule_net_classes"] = cr_classes;
  // Differential pairs (doc 05 §15): how each wanted pair came out, measured on the new copper (only when pairs are on).
  if (opt.diff_pairs || !opt.pair_nets.empty()) {
    const auto cm = drc::build_copper(lb.board);
    const drc::RuleEngine re(lb.board, rules, cm);
    auto want = opt.diff_pairs ? route::named_pairs(lb.board, re) : std::vector<std::pair<model::NetId, model::NetId>>{};
    for (const auto& p : opt.pair_nets)
      if (std::find(want.begin(), want.end(), p) == want.end() && std::find(want.begin(), want.end(), std::make_pair(p.second, p.first)) == want.end())
        want.push_back(p);
    nlohmann::json pj = nlohmann::json::array();
    log(fmt("differential pairs: %zu wanted, %d coupled pair routes (re-coupling included)", want.size(), res.pairs));
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
