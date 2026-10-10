// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal side of `tracemaker tui`: raw keys in, painted lines out, with plain termios and ANSI escape
// sequences (no curses dependency). It talks to /dev/tty, so the screen works when standard output is a pipe.
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <stdexcept>

#include "app/tui.hpp"

namespace tmk::app::tui {

namespace {

void on_resize(int) {}  // its only job is to interrupt the blocked read

void write_all(int fd, const std::string& s) {
  std::size_t at = 0;
  while (at < s.size()) {
    const ssize_t n = ::write(fd, s.data() + at, s.size() - at);
    if (n < 0 && errno != EINTR) return;
    if (n > 0) at += static_cast<std::size_t>(n);
  }
}

// Raw mode and the alternate screen for the lifetime of the object; the shell's screen comes back untouched.
class Terminal {
 public:
  Terminal() : fd_(::open("/dev/tty", O_RDWR | O_CLOEXEC)) {
    if (fd_ < 0 || ::tcgetattr(fd_, &saved_) != 0) {
      if (fd_ >= 0) ::close(fd_);
      throw std::runtime_error("tui needs a terminal (no /dev/tty); use `tracemaker route --help` for the options");
    }
    termios raw = saved_;
    ::cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(fd_, TCSAFLUSH, &raw);
    struct sigaction sa {};
    sa.sa_handler = on_resize;  // no SA_RESTART: the blocked read returns and the screen is painted again
    ::sigaction(SIGWINCH, &sa, &old_winch_);
    write_all(fd_, "\x1b[?1049h\x1b[?25l");
  }
  ~Terminal() {
    write_all(fd_, "\x1b[0m\x1b[?25h\x1b[?1049l");
    ::tcsetattr(fd_, TCSAFLUSH, &saved_);
    ::sigaction(SIGWINCH, &old_winch_, nullptr);
    ::close(fd_);
  }
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  void size(int& rows, int& cols) const {
    winsize ws{};
    if (::ioctl(fd_, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
      rows = ws.ws_row;
      cols = ws.ws_col;
    } else {
      rows = 24;
      cols = 80;
    }
  }

  void paint(const std::vector<Line>& lines) const {
    static const bool colour = std::getenv("NO_COLOR") == nullptr;
    std::string s = "\x1b[H";
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const char* on = "";
      switch (lines[i].style) {
        case Line::title: on = "\x1b[1m"; break;
        case Line::selected: on = "\x1b[7m"; break;
        case Line::set: on = "\x1b[1m"; break;
        case Line::dim: on = "\x1b[2m"; break;
        case Line::error: on = colour ? "\x1b[1;31m" : "\x1b[1m"; break;
        case Line::plain: break;
      }
      s += on;
      s += lines[i].text;
      s += "\x1b[0m\x1b[K";
      if (i + 1 < lines.size()) s += "\r\n";
    }
    write_all(fd_, s);
  }

  // The next key, or -1 when the read was interrupted (window resized).
  int read_key() const {
    unsigned char c = 0;
    if (!next(c, -1)) return -1;
    if (c != key::esc) return c == '\n' ? key::enter : c;
    // An escape sequence arrives in one burst; a lone Esc is followed by nothing.
    unsigned char a = 0, b = 0;
    if (!next(a, 50)) return key::esc;
    if (a != '[' && a != 'O') return key::esc;
    if (!next(b, 50)) return key::esc;
    switch (b) {
      case 'A': return key::up;
      case 'B': return key::down;
      case 'H': return key::home;
      case 'F': return key::end;
      default: break;
    }
    if (b >= '0' && b <= '9') {
      unsigned char t = 0;
      while (next(t, 50) && t != '~' && !(t >= 'A' && t <= 'Z')) {}
      switch (b) {
        case '1': case '7': return key::home;
        case '4': case '8': return key::end;
        case '3': return key::del;
        case '5': return key::page_up;
        case '6': return key::page_down;
        default: break;
      }
    }
    return 0;  // a sequence the screen has no use for
  }

 private:
  bool next(unsigned char& c, int timeout_ms) const {
    if (timeout_ms >= 0) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, timeout_ms) <= 0) return false;
    }
    return ::read(fd_, &c, 1) == 1;
  }

  int fd_;
  termios saved_{};
  struct sigaction old_winch_ {};
};

}  // namespace

Action run_terminal(Model& m) {
  Terminal term;
  for (;;) {
    int rows = 0, cols = 0;
    term.size(rows, cols);
    term.paint(m.render(rows, cols));
    const int k = term.read_key();
    if (k <= 0) continue;
    const Action a = m.press(k);
    if (a == Action::save)
      save_options(m);
    else if (a != Action::none)
      return a;
  }
}

}  // namespace tmk::app::tui
