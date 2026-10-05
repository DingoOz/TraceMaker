// tracemaker-view: serves the live viewer for one board, optionally with a synthetic event stream (--demo) so
// the viewer can be developed and shown without the router.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "drc/copper.hpp"
#include "io/kicad/board_reader.hpp"
#include "server/messages.hpp"
#include "server/replay_log.hpp"
#include "server/viewer_server.hpp"

namespace {

using namespace tmk;
using model::Point;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop = true; }

struct PadInfo {
  Point p;
  model::LayerMask layers = 0;
};

struct Conn {
  int a = -1, b = -1;  // indices into pads
  model::NetId net = 0;
};

// Minimum spanning trees per net (Prim, quadratic per net): the demo's "ratsnest".
std::vector<Conn> spanning_connections(const model::Board& b, const std::vector<PadInfo>& pads) {
  std::vector<std::vector<int>> by_net;
  for (std::size_t i = 0; i < b.pads.size(); ++i) {
    const auto n = b.pads[i].net;
    if (n <= 0 || pads[i].layers == 0) continue;
    if (by_net.size() <= static_cast<std::size_t>(n)) by_net.resize(static_cast<std::size_t>(n) + 1);
    by_net[static_cast<std::size_t>(n)].push_back(static_cast<int>(i));
  }
  auto d2 = [&](int i, int j) {
    const double dx = static_cast<double>(pads[static_cast<std::size_t>(i)].p.x - pads[static_cast<std::size_t>(j)].p.x);
    const double dy = static_cast<double>(pads[static_cast<std::size_t>(i)].p.y - pads[static_cast<std::size_t>(j)].p.y);
    return dx * dx + dy * dy;
  };
  std::vector<Conn> out;
  for (std::size_t n = 0; n < by_net.size(); ++n) {
    const auto& v = by_net[n];
    if (v.size() < 2 || v.size() > 400) continue;  // skip huge power nets: they are planes in practice
    std::vector<bool> in(v.size(), false);
    std::vector<double> best(v.size(), 1e300);
    std::vector<std::size_t> from(v.size(), 0);
    in[0] = true;
    for (std::size_t k = 1; k < v.size(); ++k) best[k] = d2(v[0], v[k]);
    for (std::size_t step = 1; step < v.size(); ++step) {
      std::size_t pick = 0;
      double pd = 1e301;
      for (std::size_t k = 0; k < v.size(); ++k)
        if (!in[k] && best[k] < pd) pd = best[k], pick = k;
      in[pick] = true;
      if (pd > 0) out.push_back({v[from[pick]], v[pick], static_cast<model::NetId>(n)});
      for (std::size_t k = 0; k < v.size(); ++k)
        if (!in[k]) {
          const double d = d2(v[pick], v[k]);
          if (d < best[k]) best[k] = d, from[k] = pick;
        }
    }
  }
  std::sort(out.begin(), out.end(), [&](const Conn& x, const Conn& y) { return d2(x.a, x.b) < d2(y.a, y.b); });
  return out;
}

// Octilinear two-segment path: straight along the major axis, then 45°.
Point bend_point(Point a, Point b) {
  const Coord dx = b.x - a.x, dy = b.y - a.y;
  const Coord ax = std::abs(dx), ay = std::abs(dy);
  if (ax >= ay) return {a.x + (dx > 0 ? 1 : -1) * (ax - ay), a.y};
  return {a.x, a.y + (dy > 0 ? 1 : -1) * (ay - ax)};
}

class Demo {
 public:
  Demo(server::ViewerServer& srv, const model::Board& b, std::string name) : srv_(srv), board_(b), name_(std::move(name)) {
    for (const auto& p : b.pads) {
      PadInfo info;
      const auto shapes = drc::pad_shapes(p);
      geom::Box box;
      for (const auto& s : shapes) box.add(s.box);
      info.p = box.empty() ? p.pos : Point{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
      info.layers = p.copper;
      pads_.push_back(info);
    }
  }

  void run() {
    while (!g_stop) {
      reset();
      loop();
      for (int i = 0; i < 50 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

 private:
  struct Wave {
    Point c;
    int layer = 0;
    double r = 0, speed = 0, life = 0;
    std::int64_t conn = 0;
    double dir = 0;     // direction to the target (radians); the front is elongated towards it like A*'s
    bool aimed = false;
  };
  struct Placed {
    std::vector<server::ObjectId> tracks, vias;
    Conn conn;
  };

  void reset() {
    srv_.publish(server::board_snapshot_json(board_, name_));
    pending_ = spanning_connections(board_, pads_);
    total_ = static_cast<std::int64_t>(pending_.size());
    placed_.clear();
    waves_.clear();
    next_track_ = 0;
    next_via_ = 0;
    rips_ = failures_ = iteration_ = 0;
    start_ = Clock::now();
    publish_ratsnest();
    srv_.publish(server::stage("route", "begin", "demo: synthetic events"));
    srv_.publish(server::log("info", "demo: " + std::to_string(total_) + " connections on " + name_));
  }

  void publish_ratsnest() {
    std::vector<server::RatsEdge> e;
    e.reserve(pending_.size());
    for (const auto& c : pending_) e.push_back({pads_[static_cast<std::size_t>(c.a)].p, pads_[static_cast<std::size_t>(c.b)].p, c.net});
    srv_.publish(server::ratsnest(e));
  }

  static int first_layer(model::LayerMask m) {
    for (int i = 0; i < 64; ++i)
      if (m & model::layer_bit(i)) return i;
    return 0;
  }

  // Layers of the two segments (they differ when the connection changes layer through a via).
  std::pair<int, int> pick_layers(const Conn& c) {
    const auto la = pads_[static_cast<std::size_t>(c.a)].layers, lb = pads_[static_cast<std::size_t>(c.b)].layers;
    const auto common = la & lb;
    if (common == 0) return {first_layer(la), first_layer(lb)};
    std::vector<int> ls;
    for (int i = 0; i < board_.copper_count(); ++i)
      if (common & model::layer_bit(i)) ls.push_back(i);
    const int l1 = ls[rng_() % ls.size()];
    if (ls.size() > 1 && rng_() % 5 == 0) {
      int l2 = ls[rng_() % ls.size()];
      if (l2 == l1) l2 = ls[(static_cast<std::size_t>(std::find(ls.begin(), ls.end(), l1) - ls.begin()) + 1) % ls.size()];
      return {l1, l2};
    }
    return {l1, l1};
  }

  void spawn_wave(Point c, int layer, double reach, std::int64_t conn, std::optional<Point> target = {}) {
    std::uniform_real_distribution<double> u(0.8, 1.3);
    reach = std::min(reach, 25.0 * kNmPerMm);
    Wave w{c, layer, 0.2 * kNmPerMm, std::max(reach / 8.0, 0.4 * kNmPerMm) * u(rng_), 0, conn, 0, false};
    if (target) {
      w.dir = std::atan2(static_cast<double>(target->y - c.y), static_cast<double>(target->x - c.x));
      w.aimed = true;
    }
    waves_.push_back(w);
  }

  void step_waves() {
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    for (auto& w : waves_) {
      w.r += w.speed;
      w.life += 1;
      std::vector<Point> pts;
      const int n = std::clamp(static_cast<int>(w.r / (0.35 * kNmPerMm)), 12, 72);
      for (int i = 0; i < n; ++i) {
        const double t = (i + 0.5 * jitter(rng_)) / n;  // 0..1 around the front
        double ang, rr;
        if (w.aimed) {
          // Goal-directed front: reaches furthest towards the target, little behind the source.
          const double off = (t * 2 - 1) * std::numbers::pi * 0.85;
          ang = w.dir + off;
          rr = w.r * (0.25 + 0.75 * std::pow(std::cos(off / 2), 2.0)) * (1 + 0.07 * jitter(rng_));
        } else {
          ang = t * 2 * std::numbers::pi;
          rr = w.r * (1 + 0.06 * jitter(rng_));
        }
        pts.push_back({w.c.x + static_cast<Coord>(rr * std::cos(ang)), w.c.y + static_cast<Coord>(rr * std::sin(ang))});
      }
      srv_.publish(server::frontier(w.conn, w.layer, pts));
    }
    std::erase_if(waves_, [](const Wave& w) { return w.life > 9; });
  }

  void commit(const Conn& c, std::int64_t conn_id) {
    const Point a = pads_[static_cast<std::size_t>(c.a)].p, b = pads_[static_cast<std::size_t>(c.b)].p;
    const auto [layer, second] = pick_layers(c);
    const bool via = layer != second;
    const Point m = bend_point(a, b);
    const Coord w = 250'000;
    Placed pl{{}, {}, c};
    std::vector<server::PathPoint> path = {{a, layer}, {m, layer}, {m, second}, {b, second}};
    srv_.publish(server::path_try(conn_id, path));
    auto add_track = [&](Point p, Point q, int l) {
      if (p == q) return;
      const auto id = next_track_++;
      srv_.publish(server::track_add(id, p, q, w, l, c.net));
      pl.tracks.push_back(id);
    };
    add_track(a, m, layer);
    if (via) {
      model::Via v;
      v.pos = m;
      v.size = 600'000;
      v.drill = 300'000;
      v.layer_top = 0;
      v.layer_bottom = board_.copper_count() - 1;
      v.net = c.net;
      const auto id = next_via_++;
      srv_.publish(server::via_add(id, v));
      pl.vias.push_back(id);
    }
    add_track(m, b, second);
    placed_.push_back(std::move(pl));
  }

  void fail(const Conn& c, std::int64_t conn_id) {
    server::Failure f;
    f.conn = conn_id;
    f.net = c.net;
    f.rung = static_cast<int>(rng_() % 4);
    static const char* kCauses[] = {"blocked: no path within budget", "via limit", "congestion: cut over capacity",
                                    "escape blocked by neighbouring pads"};
    f.cause = kCauses[rng_() % 4];
    f.a = pads_[static_cast<std::size_t>(c.a)].p;
    f.b = pads_[static_cast<std::size_t>(c.b)].p;
    f.region = {std::min(f.a.x, f.b.x), std::min(f.a.y, f.b.y), std::max(f.a.x, f.b.x), std::max(f.a.y, f.b.y)};
    ++failures_;
    srv_.publish(server::failure(f));
  }

  void rip() {
    if (placed_.size() < 4) return;
    const std::size_t k = rng_() % placed_.size();
    auto pl = placed_[k];
    placed_.erase(placed_.begin() + static_cast<std::ptrdiff_t>(k));
    for (auto id : pl.tracks) srv_.publish(server::track_remove(id));
    for (auto id : pl.vias) srv_.publish(server::via_remove(id));
    pending_.push_back(pl.conn);
    ++rips_;
    const auto net = static_cast<std::size_t>(pl.conn.net);
    const std::string name = net < board_.nets.size() ? board_.nets[net].name : std::to_string(pl.conn.net);
    srv_.publish(server::log("info", "rip-up: net " + name + " rerouted later"));
  }

  void publish_stats() {
    server::Stats s;
    s.stage = "route";
    s.iteration = iteration_;
    s.total = total_;
    s.unrouted = static_cast<std::int64_t>(pending_.size());
    s.routed = total_ - s.unrouted;
    s.rips = rips_;
    s.failures = failures_;
    s.elapsed_s = std::chrono::duration<double>(Clock::now() - start_).count();
    s.extra = {{"tracks", static_cast<double>(next_track_)}, {"waves", static_cast<double>(waves_.size())}};
    srv_.publish(server::stats(s));
  }

  void loop() {
    auto next = Clock::now();
    int tick = 0;
    std::int64_t conn_id = 0;
    while (!g_stop && !pending_.empty()) {
      next += std::chrono::milliseconds(33);
      std::this_thread::sleep_until(next);
      ++tick;
      step_waves();
      if (tick % 3 == 0) {  // one connection every ~100 ms
        ++iteration_;
        const Conn c = pending_.front();
        pending_.erase(pending_.begin());
        const Point a = pads_[static_cast<std::size_t>(c.a)].p, b = pads_[static_cast<std::size_t>(c.b)].p;
        const double reach = std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y));
        spawn_wave(a, pick_layers(c).first, reach, conn_id, b);
        if (rng_() % 23 == 0) {
          fail(c, conn_id);
          pending_.push_back(c);  // retried at the end
        } else {
          commit(c, conn_id);
        }
        ++conn_id;
        if (rng_() % 40 == 0) rip();
        publish_ratsnest();
      }
      if (tick % 15 == 0 && !pads_.empty()) {  // background exploration from a random pad
        const auto& p = pads_[rng_() % pads_.size()];
        int layer = 0;
        for (int i = 0; i < board_.copper_count(); ++i)
          if (p.layers & model::layer_bit(i)) layer = i;
        spawn_wave(p.p, layer, 6.0 * kNmPerMm, -1);
      }
      if (tick % 6 == 0) publish_stats();  // ~5 Hz
      if (tick > 100'000) break;
    }
    publish_stats();
    srv_.publish(server::stage("route", "end", "demo complete"));
    srv_.publish(server::log("info", "demo: all connections routed; restarting in 5 s"));
  }

  server::ViewerServer& srv_;
  const model::Board& board_;
  std::string name_;
  std::vector<PadInfo> pads_;
  std::vector<Conn> pending_;
  std::vector<Placed> placed_;
  std::vector<Wave> waves_;
  std::mt19937_64 rng_{0x7ace};
  server::ObjectId next_track_ = 0, next_via_ = 0;
  std::int64_t total_ = 0, rips_ = 0, failures_ = 0, iteration_ = 0;
  Clock::time_point start_;
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"TraceMaker live viewer server"};
  std::string board_path, host = "0.0.0.0", web;
  int port = 8766;
  bool demo = false, keep = false, loop = false;
  std::string replay;
  double speed = 1.0;
  app.add_option("board", board_path, ".kicad_pcb file to show")->required()->check(CLI::ExistingFile);
  app.add_option("--port", port, "TCP port (default 8766)")->check(CLI::Range(0, 65535));
  app.add_option("--host", host, "bind address (default 0.0.0.0)");
  app.add_option("--web", web, "directory of the built viewer (default <repo>/viewer/dist)");
  app.add_flag("--demo", demo, "stream synthetic routing events (tracks are cleared and re-routed in a loop)");
  app.add_flag("--keep-tracks", keep, "with --demo: keep the board's existing tracks and vias");
  app.add_option("--replay", replay, "replay a recording made with `tracemaker route --record FILE`")->check(CLI::ExistingFile);
  app.add_option("--speed", speed, "replay speed (1 = real time)")->check(CLI::PositiveNumber);
  app.add_flag("--loop", loop, "with --replay: start again after the end");
  CLI11_PARSE(app, argc, argv);

  try {
    auto loaded = io::read_board_file(board_path);
    auto& board = loaded.board;
    const std::string name = std::filesystem::path(board_path).stem().string();
    if (!replay.empty()) {  // the recording starts from the unrouted board
      board.tracks.clear();
      board.arcs.clear();
      board.vias.clear();
    }
    if (demo && !keep) {
      board.tracks.clear();
      board.arcs.clear();
      board.vias.clear();
    }
    server::ServerOptions opts;
    opts.host = host;
    opts.port = static_cast<std::uint16_t>(port);
    opts.web_root = web;
    server::ViewerServer srv(opts);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    srv.publish(server::board_snapshot_json(board, name));
    std::printf("tracemaker-view: %s (%zu pads, %zu tracks, %zu vias, %d copper layers)\n", name.c_str(),
                board.pads.size(), board.tracks.size(), board.vias.size(), board.copper_count());
    std::printf("serving %s  ->  %s   (web root %s)\n", srv.url().c_str(), ("ws" + srv.url().substr(4) + "ws").c_str(), srv.web_root().c_str());
    if (!std::filesystem::exists(std::filesystem::path(srv.web_root()) / "index.html"))
      std::printf("note: viewer not built; run: cd viewer && npm install && npm run build\n");
    std::printf("press Ctrl-C to stop\n");
    std::fflush(stdout);

    std::thread demo_thread;
    if (demo) demo_thread = std::thread([&] { Demo(srv, board, name).run(); });
    if (!replay.empty())
      demo_thread = std::thread([&] {
        // Recorded lines are {"t":seconds, <message fields>}; publish each message (without "t") at t / speed.
        try {
          do {
            srv.publish(server::board_snapshot_json(board, name));
            server::ReplayReader in(replay);  // plain or zstd-compressed (detected from the content)
            std::string line;
            const auto t0 = std::chrono::steady_clock::now();
            while (!g_stop && in.next_line(line)) {
              if (line.rfind("{\"t\":", 0) != 0) continue;
              const std::size_t comma = line.find(',');
              if (comma == std::string::npos) continue;
              const double t = std::atof(line.c_str() + 5);
              std::string msg = "{" + line.substr(comma + 1);
              if (msg.find("\"type\":\"board\"") != std::string::npos) continue;  // snapshot already sent
              const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t / speed));
              while (!g_stop && std::chrono::steady_clock::now() < due) std::this_thread::sleep_for(std::chrono::milliseconds(5));
              srv.publish(std::move(msg));
            }
            for (int i = 0; loop && !g_stop && i < 30; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // pause at the end
          } while (loop && !g_stop);
        } catch (const std::exception& e) {  // a damaged recording ends the replay, not the server
          std::fprintf(stderr, "tracemaker-view: replay stopped: %s\n", e.what());
          srv.publish(server::log("error", std::string("replay stopped: ") + e.what()));
        }
      });
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (demo_thread.joinable()) demo_thread.join();
    const auto c = srv.counters();
    std::printf("\nstopped: %llu messages published, %llu sent, %llu dropped, %llu clients\n",
                static_cast<unsigned long long>(c.published), static_cast<unsigned long long>(c.sent),
                static_cast<unsigned long long>(c.dropped), static_cast<unsigned long long>(c.clients_total));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "tracemaker-view: %s\n", e.what());
    return 1;
  }
  return 0;
}
