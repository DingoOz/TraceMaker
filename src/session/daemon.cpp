// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/daemon.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "core/version.hpp"
#include "session/session.hpp"
#include "session/transport.hpp"

namespace tmk::session {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

json error(const std::string& code, const std::string& message) { return {{"error", {{"code", code}, {"message", message}}}}; }

struct Held {
  std::unique_ptr<Session> s;
  std::string key, board, dir;
  std::map<int, std::string> marks;  // state -> the client's fingerprint of its board in that state
  Clock::time_point used;
};

class Daemon {
 public:
  Daemon(const DaemonOptions& o, fs::path root) : o_(o), root_(std::move(root)) {}

  json handle(const json& req, bool& stop) {
    if (!req.is_object() || !req.contains("op") || !req["op"].is_string()) return error("bad_argument", "expected {\"op\": ...}");
    const std::string op = req["op"].get<std::string>();
    try {
      if (op == "status") return status();
      if (op == "open") return open(req);
      if (op == "shutdown") {
        stop = true;
        return {{"ok", true}};
      }
      Held& h = held(req);
      h.used = Clock::now();
      if (op == "call") {
        if (!req.contains("tool") || !req["tool"].is_string()) return error("bad_argument", "call needs a tool name");
        return {{"result", h.s->call(req["tool"].get<std::string>(), req.contains("args") ? req["args"] : json::object())}};
      }
      if (op == "diff") {
        const int from = req.contains("from") && req["from"].is_number_integer() ? req["from"].get<int>() : 0;
        return h.s->copper_diff(from);
      }
      if (op == "mark") {
        if (!req.contains("fingerprint") || !req["fingerprint"].is_string()) return error("bad_argument", "mark needs a fingerprint");
        h.marks[h.s->state()] = req["fingerprint"].get<std::string>();
        return {{"ok", true}, {"state", h.s->state()}};
      }
      if (op == "close") {
        close(req["session"].get<int>());
        return {{"ok", true}};
      }
      return error("bad_argument", "unknown op: " + op);
    } catch (const std::out_of_range& e) {
      return error("bad_argument", std::string("unknown state: ") + e.what());
    } catch (const std::invalid_argument& e) {
      return error("bad_argument", e.what());
    } catch (const std::exception& e) {
      return error("failed", e.what());
    }
  }

 private:
  json status() const {
    json a = json::array();
    const auto now = Clock::now();
    for (const auto& [id, h] : held_)
      a.push_back({{"session", id}, {"key", h.key}, {"board", h.board}, {"state", h.s->state()},
                   {"idle_s", std::chrono::duration<double>(now - h.used).count()}});
    return {{"sessions", a}, {"version", std::string(tmk::version())}};
  }

  Held& held(const json& req) {
    if (!req.contains("session") || !req["session"].is_number_integer()) throw std::invalid_argument("the request needs a session id");
    const auto it = held_.find(req["session"].get<int>());
    if (it == held_.end()) throw std::invalid_argument("no session " + std::to_string(req["session"].get<int>()) + " (closed or never opened)");
    return it->second;
  }

  json open(const json& req) {
    const std::string key = req.contains("key") && req["key"].is_string() ? req["key"].get<std::string>() : "";
    const std::string fp = req.contains("fingerprint") && req["fingerprint"].is_string() ? req["fingerprint"].get<std::string>() : "";
    if (!key.empty() && !fp.empty())
      for (auto& [id, h] : held_)
        if (h.key == key) {
          const auto m = h.marks.find(h.s->state());
          if (m != h.marks.end() && m->second == fp) {
            h.used = Clock::now();
            return {{"session", id}, {"reused", true}, {"state", h.s->state()}};
          }
        }
    if (!req.contains("board") || !req["board"].is_string()) return error("bad_argument", "open needs a board (no session matches the fingerprint)");
    const fs::path board = req["board"].get<std::string>();
    if (!fs::is_regular_file(board)) return error("bad_argument", "no board file " + board.string());
    // The client's copy may be a temporary file: the session reads its own copy, with the project files that carry the rules.
    const int id = next_++;
    const fs::path dir = root_ / ("session-" + std::to_string(id));
    fs::create_directories(dir);
    const fs::path copy = dir / board.filename();
    fs::copy_file(board, copy, fs::copy_options::overwrite_existing);
    for (const char* ext : {".kicad_pro", ".kicad_dru", ".kicad_prl"}) {
      fs::path side = board;
      side.replace_extension(ext);
      if (fs::exists(side)) fs::copy_file(side, fs::path(copy).replace_extension(ext), fs::copy_options::overwrite_existing);
    }
    SessionOptions so;
    so.in = copy.string();
    so.out = req.contains("out") && req["out"].is_string() ? req["out"].get<std::string>() : "";
    so.seed = req.contains("seed") && req["seed"].is_number_unsigned() ? req["seed"].get<std::uint64_t>() : 1;
    so.work_limit = req.contains("work_limit") && req["work_limit"].is_number_integer() ? req["work_limit"].get<long>() : 0;
    so.threads = o_.threads;
    so.gpu_device = o_.use_gpu ? 0 : -1;
    Held h;
    try {
      h.s = std::make_unique<Session>(so);
    } catch (...) {
      std::error_code ec;
      fs::remove_all(dir, ec);
      throw;
    }
    h.key = key;
    h.board = board.string();
    h.dir = dir.string();
    h.used = Clock::now();
    if (!fp.empty()) h.marks[0] = fp;
    // A newer board for the same key replaces the older session (it described the board before the user's own edits).
    for (auto it = held_.begin(); it != held_.end();)
      if (!key.empty() && it->second.key == key) it = drop(it);
      else ++it;
    held_.emplace(id, std::move(h));
    while (static_cast<int>(held_.size()) > std::max(1, o_.max_sessions)) {
      auto lru = held_.begin();
      for (auto it = held_.begin(); it != held_.end(); ++it)
        if (it->second.used < lru->second.used) lru = it;
      drop(lru);
    }
    return {{"session", id}, {"reused", false}, {"state", 0}};
  }

  std::map<int, Held>::iterator drop(std::map<int, Held>::iterator it) {
    std::error_code ec;
    fs::remove_all(it->second.dir, ec);
    return held_.erase(it);
  }
  void close(int id) {
    const auto it = held_.find(id);
    if (it == held_.end()) throw std::runtime_error("no session " + std::to_string(id));
    drop(it);
  }

  DaemonOptions o_;
  fs::path root_;
  std::map<int, Held> held_;
  int next_ = 1;

 public:
  ~Daemon() {
    while (!held_.empty()) drop(held_.begin());
  }
};

// Reads one line from a socket (at most kMaxRequestBytes; longer lines are refused). Returns false at end of stream.
bool read_line(int fd, std::string& buf, std::string& line, bool& too_long) {
  too_long = false;
  for (;;) {
    if (const auto nl = buf.find('\n'); nl != std::string::npos) {
      line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (line.size() > kMaxRequestBytes) too_long = true;
      return true;
    }
    if (buf.size() > kMaxRequestBytes) {  // drop what has come of an over-long line, keep reading to its end
      too_long = true;
      buf.clear();
    }
    char tmp[65536];
    const ssize_t n = ::read(fd, tmp, sizeof tmp);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    buf.append(tmp, static_cast<std::size_t>(n));
  }
}

bool write_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);  // SIGPIPE is ignored (run_daemon)
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// socket() without SOCK_CLOEXEC, which macOS lacks; the descriptor must not leak into a child the router starts.
int cloexec_socket() {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

}  // namespace

std::string default_socket_path() {
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (rt && *rt) return (fs::path(rt) / "tracemaker" / "daemon.sock").string();
  return (fs::path("/tmp") / ("tracemaker-" + std::to_string(::getuid())) / "daemon.sock").string();
}

int run_daemon(const DaemonOptions& opt) {
  std::signal(SIGPIPE, SIG_IGN);  // a client that goes away mid-answer must not end the daemon
  const fs::path sock = opt.socket.empty() ? default_socket_path() : opt.socket;
  sockaddr_un addr{};
  if (sock.string().size() >= sizeof addr.sun_path) {
    std::fprintf(stderr, "error: socket path too long: %s\n", sock.c_str());
    return 1;
  }
  std::error_code ec;
  fs::create_directories(sock.parent_path(), ec);
  ::chmod(sock.parent_path().c_str(), 0700);  // only the owner may reach the socket
  const int fd = cloexec_socket();
  if (fd < 0) {
    std::fprintf(stderr, "error: socket: %s\n", std::strerror(errno));
    return 1;
  }
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
  if (fs::exists(sock)) {
    // A live daemon answers a connect; a stale socket file (a daemon that was killed) does not, and is replaced.
    const int probe = cloexec_socket();
    const bool live = probe >= 0 && ::connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
    if (probe >= 0) ::close(probe);
    if (live) {
      std::fprintf(stderr, "error: a daemon is already serving %s\n", sock.c_str());
      ::close(fd);
      return 1;
    }
    fs::remove(sock, ec);
  }
  const mode_t old = ::umask(0177);
  const int rc = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
  ::umask(old);
  if (rc != 0 || ::listen(fd, 8) != 0) {
    std::fprintf(stderr, "error: cannot serve %s: %s\n", sock.c_str(), std::strerror(errno));
    ::close(fd);
    return 1;
  }
  const fs::path root = sock.parent_path() / ("sessions-" + std::to_string(::getpid()));
  fs::create_directories(root, ec);
  std::fprintf(stderr, "tracemaker daemon %s serving %s (idle exit %d s)\n", std::string(tmk::version()).c_str(), sock.c_str(), opt.idle_exit_s);
  bool stop = false;
  {
    Daemon d(opt, root);
    auto last = Clock::now();
    while (!stop) {
      pollfd p{fd, POLLIN, 0};
      const int r = ::poll(&p, 1, 1000);
      if (r < 0 && errno != EINTR) break;
      if (r <= 0) {
        if (opt.idle_exit_s > 0 && std::chrono::duration<double>(Clock::now() - last).count() > opt.idle_exit_s) {
          std::fprintf(stderr, "idle for %d s: exiting\n", opt.idle_exit_s);
          break;
        }
        continue;
      }
      const int c = ::accept(fd, nullptr, nullptr);
      if (c < 0) continue;
      ::fcntl(c, F_SETFD, FD_CLOEXEC);
      std::string buf, line;
      bool too_long = false;
      while (!stop && read_line(c, buf, line, too_long)) {
        if (line.empty() && !too_long) continue;
        json ans;
        if (too_long) {
          ans = error("limit", "request line too long");
        } else {
          try {
            ans = d.handle(json::parse(line), stop);
          } catch (const json::parse_error&) {
            ans = error("bad_argument", "not JSON");
          }
        }
        if (!write_all(c, ans.dump() + "\n")) break;
      }
      ::close(c);
      last = Clock::now();
    }
  }
  ::close(fd);
  fs::remove(sock, ec);
  fs::remove_all(root, ec);
  return 0;
}

}  // namespace tmk::session
