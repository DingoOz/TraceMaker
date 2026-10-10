// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/tui.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace tmk::app::tui {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Characters of a UTF-8 string, counted as one column each (good enough for option names and file paths).
std::size_t width(const std::string& s) {
  std::size_t n = 0;
  for (char c : s) n += continuation(c) ? 0u : 1u;
  return n;
}

// The first `w` columns of `s`; with `pad`, filled with spaces up to `w`.
std::string fit(const std::string& s, std::size_t w, bool pad = false) {
  std::string out;
  std::size_t n = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!continuation(s[i])) {
      if (n == w) break;
      ++n;
    }
    out += s[i];
  }
  if (pad) out.append(w - n, ' ');
  return out;
}

// `s` over at most `lines` lines of `w` columns, broken at spaces; what does not fit ends in "...".
std::vector<std::string> wrap(const std::string& s, std::size_t w, std::size_t lines) {
  std::vector<std::string> out;
  std::string rest = s;
  while (out.size() < lines) {
    if (width(rest) <= w) {
      out.push_back(rest);
      rest.clear();
      break;
    }
    std::string head = fit(rest, w);
    const auto sp = head.find_last_of(' ');
    if (sp != std::string::npos && sp > w / 2) head.resize(sp);
    rest = trim(rest.substr(head.size()));
    out.push_back(head);
  }
  if (!rest.empty() && !out.empty() && w > 3) out.back() = fit(out.back(), w - 3) + "...";
  out.resize(lines);
  return out;
}

std::string shell_quote(const std::string& s) {
  static const std::string safe = "_@%+=:,./-";
  const bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || safe.find(c) != std::string::npos;
  });
  if (plain) return s;
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

bool is_number(const std::string& s) {
  if (s.empty() || std::isspace(static_cast<unsigned char>(s.front()))) return false;
  char* end = nullptr;
  std::strtod(s.c_str(), &end);
  // Only plain decimal numbers: strtod also takes "inf", "nan" and hexadecimal, which are names as likely as numbers.
  return *end == '\0' && s.find_first_not_of("+-.0123456789eE") == std::string::npos;
}

std::string config_value(const std::string& s) {
  if (is_number(s)) return s;
  std::string q = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') q += '\\';
    q += c;
  }
  return q + "\"";
}

// A value as an options file writes it, back to plain text: quotes off, an array's items joined by commas.
std::string plain_value(std::string v) {
  v = trim(v);
  if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
    std::string joined, item;
    char quote = 0;
    const std::string body = v.substr(1, v.size() - 2);
    auto flush = [&] {
      const std::string t = plain_value(item);
      if (!t.empty()) joined += (joined.empty() ? "" : ",") + t;
      item.clear();
    };
    for (char c : body) {
      if (quote) {
        if (c == quote) quote = 0;
        item += c;
      } else if (c == '"' || c == '\'') {
        quote = c;
        item += c;
      } else if (c == ',') {
        flush();
      } else {
        item += c;
      }
    }
    flush();
    return joined;
  }
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    std::string out;
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
      if (v[i] == '\\' && i + 2 < v.size()) ++i;
      out += v[i];
    }
    return out;
  }
  if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return v.substr(1, v.size() - 2);
  return v;
}

}  // namespace

Model::Model(std::string command, std::vector<Field> fields, Check check)
    : command_(std::move(command)), fields_(std::move(fields)), check_(std::move(check)) {
  recheck();
}

void Model::recheck() { error_ = check_ ? check_(args()) : std::string(); }

std::vector<std::size_t> Model::visible() const {
  const std::string want = lower(filter_);
  // With a filter, options whose name matches come before those that only mention the text in their description.
  std::vector<std::size_t> by_name, by_help;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    const Field& f = fields_[i];
    // A hidden option that is set stays in sight: the command line below would otherwise show what the list does not.
    if (f.hidden && !show_hidden_ && f.value.empty()) continue;
    if (want.empty() || lower(f.label).find(want) != std::string::npos)
      by_name.push_back(i);
    else if (lower(f.help).find(want) != std::string::npos)
      by_help.push_back(i);
  }
  by_name.insert(by_name.end(), by_help.begin(), by_help.end());
  return by_name;
}

std::vector<std::string> Model::args() const {
  std::vector<std::string> out;
  for (const Field& f : fields_)
    if (!f.is_flag() && f.arg.empty() && !f.value.empty()) out.push_back(f.value);
  for (const Field& f : fields_) {
    if (f.value.empty()) continue;
    if (f.is_flag()) {
      out.push_back(f.value);
    } else if (!f.arg.empty()) {
      // A value that starts with a dash would be read as another option.
      if (f.value.front() == '-') {
        out.push_back(f.arg + "=" + f.value);
      } else {
        out.push_back(f.arg);
        out.push_back(f.value);
      }
    }
  }
  return out;
}

std::string Model::command_line() const {
  std::string s = command_;
  for (const std::string& a : args()) s += " " + shell_quote(a);
  return s;
}

std::string Model::config_text() const {
  std::string s = "# TraceMaker route options, written by `tracemaker tui`.\n"
                  "# Use: tracemaker route BOARD -o OUTPUT --config THIS_FILE (options on the command line win)\n";
  for (const Field& f : fields_) {
    if (f.value.empty() || !f.save) continue;
    if (f.is_flag())
      s += f.value.substr(f.value.find_first_not_of('-')) + " = true\n";
    else if (!f.key.empty())
      s += f.key + " = " + config_value(f.value) + "\n";
  }
  return s;
}

std::vector<Entry> parse_options(const std::string& text) {
  std::vector<Entry> out;
  std::size_t at = 0;
  int number = 0;
  while (at <= text.size()) {
    const auto nl = text.find('\n', at);
    const std::string line = trim(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
    at = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++number;
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    const auto eq = line.find('=');
    std::string name = trim(line.substr(0, eq));
    name.erase(0, std::min(name.find_first_not_of('-'), name.size()));
    out.push_back({number, name, eq == std::string::npos ? "true" : plain_value(line.substr(eq + 1))});
  }
  return out;
}

int truth(const std::string& value) {
  const std::string v = lower(value);
  if (v == "true" || v == "on" || v == "yes" || v == "1") return 1;
  if (v == "false" || v == "off" || v == "no" || v == "0") return 0;
  return -1;
}

std::vector<std::string> Model::load_config(const std::string& text) {
  std::vector<std::string> problems;
  for (const Entry& e : parse_options(text)) {
    const std::string where = "line " + std::to_string(e.line) + ": ";
    bool found = false;
    for (Field& f : fields_) {
      if (!f.is_flag()) {
        if (f.key != e.name || e.name.empty()) continue;
        f.value = e.value;
        found = true;
        break;
      }
      const auto sp = std::find(f.spellings.begin(), f.spellings.end(), "--" + e.name);
      if (sp == f.spellings.end()) continue;
      const int t = truth(e.value);
      if (t == 1)
        f.value = *sp;
      else if (t == 0)  // "x = false" is the other spelling where the flag has two (--x / --no-x), else the default
        f.value = f.spellings.size() == 2 ? f.spellings[sp == f.spellings.begin() ? 1 : 0] : std::string();
      else
        problems.push_back(where + e.name + " is a flag, its value must be true or false");
      found = true;
      break;
    }
    if (!found) problems.push_back(where + "no option named " + e.name);
  }
  recheck();
  return problems;
}

bool Model::set(const std::string& name, const std::string& value) {
  for (Field& f : fields_) {
    if (f.is_flag() || (f.label != name && (f.arg.empty() || f.arg != name))) continue;
    f.value = value;
    recheck();
    return true;
  }
  return false;
}

// Line editing shared by the three prompts. True if the key changed the buffer.
bool Model::edit_key(int k) {
  if (k == key::backspace || k == 8) {
    while (!buffer_.empty() && continuation(buffer_.back())) buffer_.pop_back();
    if (!buffer_.empty()) buffer_.pop_back();
    return true;
  }
  if (k == key::ctrl_u) {
    buffer_.clear();
    return true;
  }
  if ((k >= 32 && k < 127) || (k >= 128 && k < 256)) {
    buffer_ += static_cast<char>(k);
    return true;
  }
  return false;
}

Action Model::press(int k) {
  const std::vector<std::size_t> vis = visible();
  if (sel_ >= vis.size()) sel_ = vis.empty() ? 0 : vis.size() - 1;
  if (mode_ != Mode::list) {
    if (k == key::ctrl_c) return Action::quit;
    if (k == key::esc) {
      if (mode_ == Mode::filter) filter_.clear(), sel_ = 0;
      mode_ = Mode::list;
      return Action::none;
    }
    if (k == key::enter) {
      const Mode was = mode_;
      mode_ = Mode::list;
      if (was == Mode::edit && !vis.empty()) {
        fields_[vis[sel_]].value = trim(buffer_);
        recheck();
      } else if (was == Mode::save && !trim(buffer_).empty()) {
        save_path_ = trim(buffer_);
        return Action::save;
      }
      return Action::none;
    }
    if (edit_key(k) && mode_ == Mode::filter) filter_ = buffer_, sel_ = 0;
    return Action::none;
  }

  status_.clear();
  const std::size_t last = vis.empty() ? 0 : vis.size() - 1;
  switch (k) {
    case key::up:
    case 'k':
      if (sel_ > 0) --sel_;
      break;
    case key::down:
    case 'j':
      if (sel_ < last) ++sel_;
      break;
    case key::page_up:
      sel_ = sel_ > 10 ? sel_ - 10 : 0;
      break;
    case key::page_down:
      sel_ = std::min(last, sel_ + 10);
      break;
    case key::home:
    case 'g':
      sel_ = 0;
      break;
    case key::end:
    case 'G':
      sel_ = last;
      break;
    case ' ':
    case key::enter: {
      if (vis.empty()) break;
      Field& f = fields_[vis[sel_]];
      if (f.is_flag()) {
        // Default, then each spelling in turn, then the default again.
        const auto it = std::find(f.spellings.begin(), f.spellings.end(), f.value);
        f.value = it == f.spellings.end() ? f.spellings.front() : (it + 1 == f.spellings.end() ? std::string() : *(it + 1));
        recheck();
      } else {
        buffer_ = f.value;
        mode_ = Mode::edit;
      }
      break;
    }
    case 'd':
    case key::del:
    case key::backspace:
      if (!vis.empty() && !fields_[vis[sel_]].value.empty()) {
        fields_[vis[sel_]].value.clear();
        recheck();
      }
      break;
    case '/':
      buffer_ = filter_;
      mode_ = Mode::filter;
      break;
    case 'h':
      show_hidden_ = !show_hidden_;
      break;
    case 's':
      buffer_ = save_path_;
      mode_ = Mode::save;
      break;
    case 'p':
      return Action::print;
    case 'r':
      if (error_.empty()) return Action::run;
      status_ = "cannot run yet: " + error_;
      break;
    case key::esc:  // clears the filter; it does not quit, so a stray Esc cannot lose the choices
      filter_.clear();
      sel_ = 0;
      break;
    case 'q':
    case key::ctrl_c:
      return Action::quit;
    default:
      break;
  }
  return Action::none;
}

std::vector<Line> Model::render(int rows, int cols) const {
  const std::size_t w = static_cast<std::size_t>(std::max(cols, 20));
  const std::size_t h = static_cast<std::size_t>(std::max(rows, 1));
  const std::vector<std::size_t> vis = visible();
  const std::size_t sel = std::min(sel_, vis.empty() ? 0 : vis.size() - 1);
  constexpr std::size_t kHeader = 2, kFooter = 7;
  const std::size_t body = h > kHeader + kFooter ? h - kHeader - kFooter : 1;
  if (sel < top_) top_ = sel;
  if (sel >= top_ + body) top_ = sel + 1 - body;
  if (top_ + body > vis.size()) top_ = vis.size() > body ? vis.size() - body : 0;

  std::size_t chosen = 0, hidden = 0, name_w = 0;
  for (const Field& f : fields_) chosen += f.value.empty() ? 0u : 1u, hidden += f.hidden ? 1u : 0u;
  for (std::size_t i : vis) name_w = std::max(name_w, width(fields_[i].label));
  name_w = std::min(name_w, w / 2);

  std::vector<Line> out;
  out.push_back({fit(command_ + " options: " + std::to_string(chosen) + " set, " + std::to_string(vis.size()) + " of " +
                         std::to_string(fields_.size()) + " shown",
                     w),
                 Line::title});
  std::string second;
  if (mode_ == Mode::filter)
    second = "filter: " + buffer_ + "_";
  else if (!filter_.empty())
    second = "filter: " + filter_ + "  (Esc clears)";
  else if (!show_hidden_ && hidden > 0)
    second = std::to_string(hidden) + " experimental options hidden (h shows them)";
  out.push_back({fit(second, w), Line::dim});

  for (std::size_t r = 0; r < body; ++r) {
    if (top_ + r >= vis.size()) {
      out.push_back({vis.empty() && r == 0 ? "  no option matches" : "", Line::dim});
      continue;
    }
    const Field& f = fields_[vis[top_ + r]];
    std::string v;
    if (f.is_flag())
      v = f.value.empty() ? "[ ]" : (f.spellings.size() > 1 ? "[x] " + f.value : "[x]");
    else if (!f.value.empty())
      v = f.value;
    else
      v = f.def.empty() ? "-" : "(default " + f.def + ")";
    const bool cur = top_ + r == sel;
    const std::string text = std::string(cur ? "> " : "  ") + fit(f.label, name_w, true) + "  " + v;
    out.push_back({fit(text, w, cur), cur ? Line::selected : (f.value.empty() ? Line::dim : Line::set)});
  }

  out.push_back({std::string(w, '-'), Line::dim});
  std::string help;
  if (!vis.empty()) {
    const Field& f = fields_[vis[sel]];
    help = f.label + ": " + (f.help.empty() ? "(no description)" : f.help);
    if (!f.def.empty()) help += " [default " + f.def + "]";
  }
  for (const std::string& l : wrap(help, w, 2)) out.push_back({l, Line::plain});
  for (const std::string& l : wrap("$ " + command_line(), w, 2)) out.push_back({l, Line::set});
  if (mode_ == Mode::edit && !vis.empty())
    out.push_back({fit(fields_[vis[sel]].label + " = " + buffer_ + "_   (Enter keeps, Esc cancels, empty = default)", w), Line::title});
  else if (mode_ == Mode::save)
    out.push_back({fit("save options to: " + buffer_ + "_", w), Line::title});
  else if (!status_.empty())
    out.push_back({fit(status_, w), status_.rfind("cannot", 0) == 0 ? Line::error : Line::title});
  else if (!error_.empty())
    out.push_back({fit("not runnable: " + error_, w), Line::error});
  else
    out.push_back({"ready to run", Line::dim});
  out.push_back({fit("Space change  d default  / filter  h hidden  s save  p print  r run  q quit", w), Line::dim});
  out.resize(h);
  return out;
}

void save_options(Model& m) {
  std::ofstream f(m.save_path());
  f << m.config_text();
  f.close();
  m.set_status(f ? "saved to " + m.save_path() : "cannot write " + m.save_path() + ": " + std::strerror(errno));
}

Action run_keys(Model& m, const std::string& keys) {
  static const std::pair<const char*, int> names[] = {
      {"up", key::up},       {"down", key::down}, {"pgup", key::page_up}, {"pgdn", key::page_down},
      {"home", key::home},   {"end", key::end},   {"enter", key::enter},  {"space", ' '},
      {"esc", key::esc},     {"bs", key::backspace}, {"del", key::del},   {"lt", '<'}};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    int k = static_cast<unsigned char>(keys[i]);
    if (keys[i] == '<') {
      const auto close = keys.find('>', i);
      const std::string name = close == std::string::npos ? std::string() : keys.substr(i + 1, close - i - 1);
      for (const auto& [n, code] : names)
        if (name == n) k = code, i = close;
    }
    const Action a = m.press(k);
    if (a == Action::save)
      save_options(m);
    else if (a != Action::none)
      return a;
  }
  return Action::print;
}

}  // namespace tmk::app::tui
