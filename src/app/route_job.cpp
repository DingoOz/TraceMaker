#include "app/route_job.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "crules/engine.hpp"
#include "gpu/device.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "learn/knowledge_base.hpp"
#include "server/messages.hpp"
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

// Records router events (JSON lines with a time stamp) for replay, e.g. comparison videos.
class FileSink final : public events::Sink {
 public:
  explicit FileSink(const std::string& path) : f_(path), t0_(std::chrono::steady_clock::now()) {}
  void publish(std::string json) override {
    if (json.size() < 2 || json.front() != '{') return;
    if (json.rfind("{\"t\":", 0) == 0) {  // already time-stamped (buffered portfolio replay)
      std::lock_guard<std::mutex> lk(m_);
      f_ << json << "\n";
      return;
    }
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    std::lock_guard<std::mutex> lk(m_);
    f_ << "{\"t\":" << t << "," << json.substr(1) << "\n";
  }
  bool wants_transient() const override { return false; }

 private:
  std::ofstream f_;
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
  const auto rules = io::read_design_rules(job.in);
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
  const model::Board* route_board = &lb.board;
  if (cr_mode != crules::Mode::Off) {
    const auto& cat = crules::builtin_catalogue();
    const auto det = crules::detect(lb.board, cat);
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
    // USB2-02 (P3): route each detected USB 2.0 D+/D- pair coupled first (a soft preference: the router falls back
    // to single tracks). Only pairs bound to exactly one net each.
    if (cr_mode == crules::Mode::Soft || cr_mode == crules::Mode::On)
      for (const auto& p : crules::usb_pairs(lb.board, cat, det)) {
        if (std::find(opt.pair_nets.begin(), opt.pair_nets.end(), p) != opt.pair_nets.end()) continue;
        opt.pair_nets.push_back(p);
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
  if (job.threads > 1) {
    std::vector<int> pick;
    if (kb && job.threads < route::portfolio_size()) pick = kb->choose_variants(feat, route::portfolio_size(), job.threads, opt.seed);
    auto pr = route::route_portfolio(*route_board, rules, opt, job.threads, pick);
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
  out.items = items_json(lb.board, res);
  if (!job.items_out.empty()) std::ofstream(job.items_out) << out.items.dump();
  out.summary = {{"routed", res.routed},     {"connections", res.connections}, {"tracks", res.tracks.size()},
                 {"vias", res.vias.size()},  {"seconds", res.seconds},         {"expansions", res.expansions},
                 {"pitch_mm", nm_to_mm(res.pitch)}, {"failures", res.failures}, {"variant", best_index}, {"variant_name", best_name},
                 {"escape_corridors", res.escape_corridors}};
  if (!job.json_out.empty()) std::ofstream(job.json_out) << out.summary.dump(1);
  if (server && job.hold) {
    log("routing finished; viewer still serving at " + server->url() + " (Ctrl-C to quit)");
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  opt.sink = nullptr;
  return out;
}

}  // namespace tmk::app
