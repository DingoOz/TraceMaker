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

  // Colours are 256-colour SGR codes; without colour (NO_COLOR) the same structure is drawn with bold, dim and reverse.
  static const char* sgr(Style st, bool colour) {
    if (colour) {
      switch (st) {
        case Style::plain: return "";
        case Style::frame: return "\x1b[38;5;67m";
        case Style::title: return "\x1b[1;38;5;231;48;5;24m";
        case Style::heading: return "\x1b[1;38;5;75m";
        case Style::dim: return "\x1b[38;5;245m";
        case Style::label: return "\x1b[38;5;252m";
        case Style::key: return "\x1b[1;38;5;221m";
        case Style::code: return "\x1b[38;5;150m";
        case Style::ok: return "\x1b[38;5;114m";
        case Style::warn: return "\x1b[38;5;215m";
        case Style::error: return "\x1b[1;38;5;203m";
        case Style::accent: return "\x1b[1;38;5;117m";
        case Style::global: return "\x1b[1;38;5;176m";
        case Style::project: return "\x1b[1;38;5;114m";
        case Style::file: return "\x1b[1;38;5;80m";
        case Style::run: return "\x1b[1;38;5;179m";
        case Style::tag_global: return "\x1b[1;38;5;16;48;5;176m";
        case Style::tag_project: return "\x1b[1;38;5;16;48;5;114m";
        case Style::tag_file: return "\x1b[1;38;5;16;48;5;80m";
        case Style::tag_run: return "\x1b[1;38;5;16;48;5;179m";
      }
      return "";
    }
    switch (st) {
      case Style::title: case Style::heading: case Style::key: case Style::accent: case Style::error:
      case Style::global: case Style::project: case Style::file: case Style::run: return "\x1b[1m";
      case Style::tag_global: case Style::tag_project: case Style::tag_file: case Style::tag_run: return "\x1b[7m";
      case Style::frame: case Style::dim: return "\x1b[2m";
      default: return "";
    }
  }

  void paint(const std::vector<Line>& lines) const {
    static const bool colour = std::getenv("NO_COLOR") == nullptr;
    std::string s = "\x1b[H";
    for (std::size_t i = 0; i < lines.size(); ++i) {
      // The current row has a background in colour, or is bold without.
      const char* row = lines[i].highlight ? (colour ? "\x1b[48;5;238m" : "\x1b[1m") : "";
      for (const Span& sp : lines[i].spans) {
        s += "\x1b[0m";
        s += row;
        s += sgr(sp.style, colour);
        s += sp.text;
      }
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
      case 'Z': return key::shift_tab;
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
