// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/live.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "core/version.hpp"

namespace tmk::live {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kLogLines = 200;
constexpr std::size_t kHistory = 720;  // samples kept; when full, every other one goes and the step doubles

struct NamedSlot {
  std::string name;
  Slot s;
};

// Lives until the process ends (never destroyed: the writer thread is detached and may still run during exit).
struct State {
  std::mutex mu;  // everything below except the slots' atomics
  std::condition_variable cv;
  bool stop = false, written = false;
  std::string path, tmp;
  int pid = 0;
  std::string proc_start, cwd, exe, argv_json, run, board;
  double started = 0;
  std::chrono::steady_clock::time_point t0;
  std::vector<std::pair<std::string, std::string>> fields;
  std::deque<std::string> log;
  std::deque<std::unique_ptr<NamedSlot>> slots;  // deque: pointers stay valid as it grows
  std::vector<int> h_routed, h_total;
  std::vector<long> h_work;
  std::vector<double> h_cpu;
  int h_step = 1, h_tick = 0;
};

State* g = nullptr;
std::atomic<bool> g_on{false};

std::string esc(const std::string& s) {
  std::string o = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o + "\"";
}

double unix_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

double cpu_seconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + 1e-6 * static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}

long rss_kb() {
  std::ifstream f("/proc/self/statm");
  long pages = 0, resident = 0;
  f >> pages >> resident;
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

// Field 22 of /proc/self/stat: when the process started (clock ticks since boot). The page uses it to tell this process from a
// later one that was given the same pid.
std::string proc_start_ticks() {
  std::ifstream f("/proc/self/stat");
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto rp = s.rfind(')');
  if (rp == std::string::npos) return "";
  std::size_t i = rp + 2;
  for (int field = 3; field < 22 && i < s.size(); ++field) i = s.find(' ', i) + 1;
  return s.substr(i, s.find(' ', i) - i);
}

template <class T>
std::string array(const std::vector<T>& v) {
  std::string o = "[";
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) o += ',';
    if constexpr (std::is_floating_point_v<T>) {
      char b[32];
      std::snprintf(b, sizeof b, "%.1f", v[i]);
      o += b;
    } else {
      o += std::to_string(v[i]);
    }
  }
  return o + "]";
}

void sample(State& s) {
  if (s.h_tick++ % s.h_step) return;
  int routed = -1, total = 0;
  long work = 0;
  for (const auto& ns : s.slots) {
    const int ph = ns->s.phase.load(std::memory_order_relaxed);
    if (ph >= 2) routed = std::max(routed, ns->s.routed.load(std::memory_order_relaxed));  // a router in setup has routed nothing yet
    total = std::max(total, ns->s.total.load(std::memory_order_relaxed));
    work += ns->s.work_done.load(std::memory_order_relaxed) + ns->s.work.load(std::memory_order_relaxed);
  }
  if (routed < 0) routed = s.h_routed.empty() ? 0 : s.h_routed.back();  // every router between runs: the last value holds
  s.h_routed.push_back(routed), s.h_total.push_back(total), s.h_work.push_back(work), s.h_cpu.push_back(cpu_seconds());
  if (s.h_routed.size() >= kHistory) {
    auto halve = [](auto& v) {
      for (std::size_t i = 0; 2 * i < v.size(); ++i) v[i] = v[2 * i];
      v.resize((v.size() + 1) / 2);
    };
    halve(s.h_routed), halve(s.h_total), halve(s.h_work), halve(s.h_cpu);
    s.h_step *= 2;
  }
}

// The status as JSON; called with s.mu held.
std::string render(State& s, const char* state, const int* exit_code) {
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
  char head[512];
  std::snprintf(head, sizeof head,
                "{\"pid\":%d,\"ppid\":%d,\"proc_start\":%s,\"started\":%.3f,\"updated\":%.3f,\"elapsed_s\":%.1f,\"state\":\"%s\",\"exit\":%s,"
                "\"cpu_s\":%.1f,\"rss_kb\":%ld,",
                s.pid, static_cast<int>(::getppid()), s.proc_start.empty() ? "null" : s.proc_start.c_str(), s.started, unix_now(), elapsed, state,
                exit_code ? std::to_string(*exit_code).c_str() : "null", cpu_seconds(), rss_kb());
  std::string o = head;
  o += "\"version\":" + esc(std::string(version())) + ",\"argv\":" + s.argv_json + ",\"cwd\":" + esc(s.cwd) + ",\"exe\":" + esc(s.exe) +
       ",\"run\":" + esc(s.run) + ",\"board\":" + esc(s.board) + ",\"fields\":[";
  for (std::size_t i = 0; i < s.fields.size(); ++i) o += (i ? ",[" : "[") + esc(s.fields[i].first) + "," + esc(s.fields[i].second) + "]";
  o += "],\"slots\":[";
  for (std::size_t i = 0; i < s.slots.size(); ++i) {
    const auto& ns = *s.slots[i];
    const long work = ns.s.work.load(std::memory_order_relaxed);
    char b[320];
    std::snprintf(b, sizeof b, "%s{\"name\":%s,\"routed\":%d,\"total\":%d,\"work\":%ld,\"work_all\":%ld,\"budget\":%ld,\"phase\":%d,\"runs\":%d}",
                  i ? "," : "", esc(ns.name).c_str(), ns.s.routed.load(std::memory_order_relaxed), ns.s.total.load(std::memory_order_relaxed), work,
                  ns.s.work_done.load(std::memory_order_relaxed) + work, ns.s.budget.load(std::memory_order_relaxed), ns.s.phase.load(std::memory_order_relaxed),
                  ns.s.runs.load(std::memory_order_relaxed));
    o += b;
  }
  char hs[64];
  std::snprintf(hs, sizeof hs, "],\"history\":{\"step_s\":%d,", s.h_step);
  o += hs;
  o += "\"routed\":" + array(s.h_routed) + ",\"total\":" + array(s.h_total) + ",\"work\":" + array(s.h_work) + ",\"cpu_s\":" + array(s.h_cpu) + "},\"log\":[";
  for (std::size_t i = 0; i < s.log.size(); ++i) o += (i ? "," : "") + esc(s.log[i]);
  return o + "]}\n";
}

void write(State& s, const char* state, const int* exit_code) {
  const std::string body = render(s, state, exit_code);
  {
    std::ofstream f(s.tmp, std::ios::trunc);
    if (!f) return;
    f << body;
    if (!f.flush()) return;
  }
  std::error_code ec;
  fs::rename(s.tmp, s.path, ec);
  s.written = !ec;
}

void writer(State* s) {
  std::unique_lock lk(s->mu);
  while (!s->stop) {
    s->cv.wait_for(lk, std::chrono::seconds(1));
    if (s->stop) break;
    sample(*s);
    write(*s, "running", nullptr);
  }
}

fs::path live_dir() {
  if (const char* d = std::getenv("TM_LIVE_DIR"); d && *d) return d;
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return fs::path(x) / "tracemaker" / "live";
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".cache" / "tracemaker" / "live";
  return {};
}

void prune(const fs::path& dir) {  // files of processes that ended over a week ago
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::error_code e2;
    const auto t = fs::last_write_time(e.path(), e2);
    if (!e2 && now - t > std::chrono::hours(24 * 7)) fs::remove(e.path(), e2);
  }
}

void stop_with(const char* state, const int* exit_code) {
  if (!g_on.exchange(false)) return;
  std::lock_guard lk(g->mu);
  g->stop = true;
  g->cv.notify_all();
  if (g->written) write(*g, state, exit_code);  // a process that ended within its first second leaves no file
}

}  // namespace

void start(int argc, char** argv) {
  if (g) return;
  if (const char* e = std::getenv("TM_LIVE"); e && std::string(e) == "0") return;
  const fs::path dir = live_dir();
  std::error_code ec;
  if (dir.empty() || (fs::create_directories(dir, ec), ec)) return;
  prune(dir);
  g = new State;
  g->pid = static_cast<int>(::getpid());
  g->path = (dir / (std::to_string(g->pid) + ".json")).string();
  g->tmp = g->path + ".tmp";
  g->proc_start = proc_start_ticks();
  g->started = unix_now();
  g->t0 = std::chrono::steady_clock::now();
  g->cwd = fs::current_path(ec).string();
  g->exe = fs::read_symlink("/proc/self/exe", ec).string();
  g->argv_json = "[";
  for (int i = 0; i < argc; ++i) g->argv_json += (i ? "," : "") + esc(argv[i]);
  g->argv_json += "]";
  if (const char* r = std::getenv("TM_LIVE_RUN")) g->run = r;  // set by bench/run.py: the run and board this process belongs to
  if (const char* b = std::getenv("TM_LIVE_BOARD")) g->board = b;
  g_on = true;
  std::atexit([] { stop_with("ended", nullptr); });
  std::thread(writer, g).detach();
}

void finish(int exit_code) { stop_with("done", &exit_code); }

bool enabled() { return g_on.load(); }

Slot* slot(const std::string& name) {
  if (!enabled()) return nullptr;
  std::lock_guard lk(g->mu);
  for (auto& ns : g->slots)
    if (ns->name == name) return &ns->s;
  g->slots.push_back(std::make_unique<NamedSlot>());
  g->slots.back()->name = name;
  return &g->slots.back()->s;
}

void field(const std::string& key, const std::string& value) {
  if (!enabled()) return;
  std::lock_guard lk(g->mu);
  for (auto it = g->fields.begin(); it != g->fields.end(); ++it)
    if (it->first == key) {
      if (value.empty()) g->fields.erase(it);
      else it->second = value;
      return;
    }
  if (!value.empty()) g->fields.emplace_back(key, value);
}

void log(const std::string& line) {
  if (!enabled()) return;
  std::lock_guard lk(g->mu);
  g->log.push_back(line);
  if (g->log.size() > kLogLines) g->log.pop_front();
}

void eprintf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fputs(buf, stderr);
  std::string line = buf;
  while (!line.empty() && line.back() == '\n') line.pop_back();
  log(line);
}

}  // namespace tmk::live
