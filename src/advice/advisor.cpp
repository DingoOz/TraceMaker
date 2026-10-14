// SPDX-License-Identifier: GPL-3.0-or-later
// Running an advisor process and reading its advice (doc 17 §5).
#include "advice/advisor.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace tmk::advice {

namespace {

double since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

void close_fd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

// pipe2 is not on every platform this builds for.
int make_pipe(int fds[2]) {
  if (::pipe(fds) != 0) return -1;
  for (int i = 0; i < 2; ++i) ::fcntl(fds[i], F_SETFD, ::fcntl(fds[i], F_GETFD) | FD_CLOEXEC);
  return 0;
}

}  // namespace

ProcessResult run_process(const std::string& command, const std::string& input, double timeout_s, std::size_t max_out, std::size_t max_err) {
  ProcessResult r;
  const auto t0 = std::chrono::steady_clock::now();
  int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
  if (make_pipe(in_p) != 0 || make_pipe(out_p) != 0 || make_pipe(err_p) != 0) {
    for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1], err_p[0], err_p[1]})
      if (fd >= 0) ::close(fd);
    r.err = std::string("pipe: ") + std::strerror(errno);
    return r;
  }
  // A child that exits without reading its input must not kill this process with SIGPIPE.
  struct sigaction ign {}, old {};
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  ::sigaction(SIGPIPE, &ign, &old);
  const pid_t pid = ::fork();
  if (pid < 0) {
    r.err = std::string("fork: ") + std::strerror(errno);
    for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1], err_p[0], err_p[1]}) ::close(fd);
    ::sigaction(SIGPIPE, &old, nullptr);
    return r;
  }
  if (pid == 0) {  // child: only async-signal-safe calls until exec
    ::setsid();    // own process group: the timeout kills whatever the shell started
    struct sigaction dfl {};  // an ignored signal stays ignored across exec: give the advisor the SIGPIPE it expects
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(SIGPIPE, &dfl, nullptr);
    ::dup2(in_p[0], 0);
    ::dup2(out_p[1], 1);
    ::dup2(err_p[1], 2);
    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  r.started = true;
  close_fd(in_p[0]);
  close_fd(out_p[1]);
  close_fd(err_p[1]);
  int in_fd = in_p[1], out_fd = out_p[0], err_fd = err_p[0];
  ::fcntl(in_fd, F_SETFL, ::fcntl(in_fd, F_GETFL) | O_NONBLOCK);
  std::size_t written = 0;
  if (input.empty()) close_fd(in_fd);
  bool killed = false;
  auto kill_group = [&] {
    if (!killed) ::kill(-pid, SIGKILL);
    killed = true;
  };
  char buf[16384];
  while (out_fd >= 0 || err_fd >= 0) {
    const double left = timeout_s - since(t0);
    if (left <= 0) {
      r.timed_out = true;
      kill_group();
      break;
    }
    struct pollfd fds[3];
    nfds_t n = 0;
    int which[3];
    if (in_fd >= 0) fds[n] = {in_fd, POLLOUT, 0}, which[n++] = 0;
    if (out_fd >= 0) fds[n] = {out_fd, POLLIN, 0}, which[n++] = 1;
    if (err_fd >= 0) fds[n] = {err_fd, POLLIN, 0}, which[n++] = 2;
    const int pr = ::poll(fds, n, static_cast<int>(std::min(left, 3600.0) * 1000) + 1);
    if (pr < 0 && errno == EINTR) continue;
    if (pr < 0) break;
    for (nfds_t k = 0; k < n; ++k) {
      if (!fds[k].revents) continue;
      if (which[k] == 0) {
        const ssize_t w = ::write(in_fd, input.data() + written, std::min<std::size_t>(input.size() - written, 65536));
        if (w > 0) written += static_cast<std::size_t>(w);
        if ((w < 0 && errno != EAGAIN && errno != EINTR) || written >= input.size()) close_fd(in_fd);
      } else {
        int& fd = which[k] == 1 ? out_fd : err_fd;
        std::string& dst = which[k] == 1 ? r.out : r.err;
        const std::size_t cap = which[k] == 1 ? max_out : max_err;
        const ssize_t g = ::read(fd, buf, sizeof buf);
        if (g > 0) {
          const std::size_t room = dst.size() < cap ? cap - dst.size() : 0;
          dst.append(buf, std::min<std::size_t>(static_cast<std::size_t>(g), room));
          if (static_cast<std::size_t>(g) > room && which[k] == 1) r.out_truncated = true;  // keep reading so the child never blocks on a full pipe
        } else if (g == 0 || (errno != EAGAIN && errno != EINTR)) {
          close_fd(fd);
        }
      }
    }
  }
  close_fd(in_fd);
  close_fd(out_fd);
  close_fd(err_fd);
  // Closed its output: it should be on its way out; wait for it, but not past the limit.
  int status = 0;
  for (;;) {
    const pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) break;
    if (since(t0) > timeout_s) {
      r.timed_out = true;
      kill_group();
      ::waitpid(pid, &status, 0);
      break;
    }
    ::usleep(5000);
  }
  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  if (killed) ::kill(-pid, SIGKILL);  // the group may hold stragglers whose leader already exited
  ::sigaction(SIGPIPE, &old, nullptr);
  r.seconds = since(t0);
  return r;
}

Consulted consult_advisor(const AdvisorOptions& opt, const nlohmann::json& report, const AliasMap& aliases, const std::string& advice_name) {
  Consulted c;
  // The program name only: a command line may hold a key or a token.
  std::string program = opt.command.substr(0, opt.command.find_first_of(" \t"));
  program = program.substr(program.find_last_of('/') == std::string::npos ? 0 : program.find_last_of('/') + 1);
  const std::string input = report.dump();
  const auto pr = run_process(opt.command, input, opt.timeout_s);
  c.summary = {{"program", program}, {"seconds", pr.seconds}, {"exit_code", pr.exit_code}, {"timed_out", pr.timed_out},
               {"bytes_in", input.size()}, {"bytes_out", pr.out.size()}};
  if (!pr.err.empty()) c.summary["stderr"] = pr.err.substr(0, 2000);
  auto fail = [&](const std::string& why) {
    c.error = why;
    c.summary["ok"] = false;
    c.summary["error"] = why;
    return c;
  };
  if (!pr.started) return fail("the advisor could not be started: " + pr.err);
  if (pr.timed_out) return fail("the advisor did not answer within " + std::to_string(static_cast<int>(opt.timeout_s)) + " s");
  if (pr.exit_code != 0) return fail("the advisor exited with status " + std::to_string(pr.exit_code));
  if (pr.out_truncated) return fail("the advisor wrote more than 1 MB");
  c.text = pr.out;
  try {
    c.advice = parse_advice(pr.out, advice_name);
    unalias(c.advice, aliases);
  } catch (const std::exception& e) {
    return fail(std::string("the advisor's answer was refused: ") + e.what());
  }
  c.ok = true;
  c.summary["ok"] = true;
  return c;
}

}  // namespace tmk::advice
