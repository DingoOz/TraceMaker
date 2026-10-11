// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/tui.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace tmk::app::tui {

const char* scope_name(Scope s) {
  switch (s) {
    case Scope::global: return "global";
    case Scope::project: return "project";
    case Scope::file: return "file";
    case Scope::run: break;
  }
  return "this run";
}

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

std::size_t display_width(const std::string& s) { return width(s); }

namespace {

// What each scope is for, said when the user moves to it.
const char* scope_note(Scope s) {
  switch (s) {
    case Scope::global: return "options for every board of this user";
    case Scope::project: return "options for the boards of this folder, over the global ones";
    case Scope::file: return "the options file given with --config, over the others";
    case Scope::run: break;
  }
  return "";
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

std::string Model::config_text(Scope sc) const {
  std::string s = std::string("# TraceMaker route options (") + scope_name(sc) +
                  "), written by `tracemaker tui`.\n"
                  "# Use: tracemaker route BOARD -o OUTPUT --config THIS_FILE (options on the command line win)\n";
  for (const Field& f : fields_) {
    const std::string& v = f.layer[static_cast<int>(sc)];
    if (v.empty() || !f.save) continue;
    if (f.is_flag())
      s += v.substr(v.find_first_not_of('-')) + " = true\n";
    else if (!f.key.empty())
      s += f.key + " = " + config_value(v) + "\n";
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

std::vector<std::string> Model::load_config(const std::string& text, Scope sc) {
  std::vector<std::string> problems;
  const int at = static_cast<int>(sc);
  for (const Entry& e : parse_options(text)) {
    const std::string where = "line " + std::to_string(e.line) + ": ";
    bool found = false;
    for (Field& f : fields_) {
      if (!f.is_flag()) {
        if (f.key != e.name || e.name.empty()) continue;
        f.layer[at] = e.value;
        found = true;
        break;
      }
      const auto sp = std::find(f.spellings.begin(), f.spellings.end(), "--" + e.name);
      if (sp == f.spellings.end()) continue;
      const int t = truth(e.value);
      if (t == 1)
        f.layer[at] = *sp;
      else if (t == 0)  // "x = false" is the other spelling where the flag has two (--x / --no-x), else the default
        f.layer[at] = f.spellings.size() == 2 ? f.spellings[sp == f.spellings.begin() ? 1 : 0] : std::string();
      else
        problems.push_back(where + e.name + " is a flag, its value must be true or false");
      found = true;
      break;
    }
    if (!found) problems.push_back(where + "no option named " + e.name);
  }
  for (Field& f : fields_) refresh(f);
  recheck();
  return problems;
}

void Model::refresh(Field& f) const {
  f.value.clear();
  f.scope = Scope::run;
  for (int i = kScopes - 1; i >= 0; --i) {
    if (f.layer[i].empty()) continue;
    f.value = f.layer[i];
    f.scope = static_cast<Scope>(i);
    break;
  }
}

void Model::put(Field& f, std::string value) {
  const Scope home_scope = home(f);
  f.layer[static_cast<int>(home_scope)] = std::move(value);
  dirty_[static_cast<int>(home_scope)] = home_scope != Scope::run;
  refresh(f);
  recheck();
  // A higher scope with a value of its own hides what was just set.
  if (!f.value.empty() && f.scope != home_scope && home_scope != Scope::run)
    status_ = std::string("kept in ") + scope_name(home_scope) + ", but the " + scope_name(f.scope) + " value wins";
}

void Model::enable_scope(Scope s, std::string path, bool exists) {
  const int i = static_cast<int>(s);
  enabled_[i] = true;
  path_[i] = std::move(path);
  exists_[i] = exists;
}

void Model::disable_scope(Scope s) {
  enabled_[static_cast<int>(s)] = false;
  if (target_ == s) next_target(1);
}

void Model::set_target(Scope s) {
  if (s != Scope::run && enabled_[static_cast<int>(s)]) target_ = s;
}

void Model::next_target(int step) {
  for (int n = 1; n <= kScopes; ++n) {
    const int i = ((static_cast<int>(target_) - 1 + step * n) % 3 + 3) % 3 + 1;  // global, project, file in a ring
    if (enabled_[i]) {
      target_ = static_cast<Scope>(i);
      return;
    }
  }
}

void Model::mark_saved() {
  dirty_[static_cast<int>(target_)] = false;
  exists_[static_cast<int>(target_)] = true;
}

bool Model::set(const std::string& name, const std::string& value) {
  for (Field& f : fields_) {
    if (f.is_flag() || (f.label != name && (f.arg.empty() || f.arg != name))) continue;
    f.layer[static_cast<int>(f.save ? target_ : Scope::run)] = value;
    refresh(f);
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
        put(fields_[vis[sel_]], trim(buffer_));
      } else if (was == Mode::save && !trim(buffer_).empty()) {
        path_[static_cast<int>(target_)] = trim(buffer_);
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
        // Default, then each spelling in turn, then the default again (in the scope that edits go to).
        const std::string& own = f.layer[static_cast<int>(home(f))];
        const auto it = std::find(f.spellings.begin(), f.spellings.end(), own);
        put(f, it == f.spellings.end() ? f.spellings.front() : (it + 1 == f.spellings.end() ? std::string() : *(it + 1)));
      } else {
        buffer_ = f.value;
        mode_ = Mode::edit;
      }
      break;
    }
    case 'd':
    case key::del:
    case key::backspace:
      if (vis.empty()) break;
      if (Field& f = fields_[vis[sel_]]; !f.layer[static_cast<int>(home(f))].empty()) {
        put(f, std::string());
      } else if (!f.value.empty()) {
        status_ = std::string("cannot reset here: the value is set in ") + scope_name(f.scope) + " (Tab moves there)";
      }
      break;
    case key::tab:
      next_target(1);
      status_ = std::string("changes now go to ") + scope_name(target_) + ": " + scope_note(target_);
      break;
    case key::shift_tab:
      next_target(-1);
      status_ = std::string("changes now go to ") + scope_name(target_) + ": " + scope_note(target_);
      break;
    case '/':
      buffer_ = filter_;
      mode_ = Mode::filter;
      break;
    case 'h':
      show_hidden_ = !show_hidden_;
      break;
    case 's':
      buffer_ = save_path();
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

namespace {

using Spans = std::vector<Span>;

const char* const kH = "\xe2\x94\x80";  // ─ (box-drawing characters are three bytes, one column)
const char* const kV = "\xe2\x94\x82";  // │

std::size_t spans_width(const Spans& sp) {
  std::size_t n = 0;
  for (const Span& s : sp) n += width(s.text);
  return n;
}

// The first `w` columns of the spans.
Spans clip(const Spans& sp, std::size_t w) {
  Spans out;
  for (const Span& s : sp) {
    if (w == 0) break;
    const std::size_t sw = width(s.text);
    out.push_back({sw <= w ? s.text : fit(s.text, w), s.style});
    w -= std::min(w, sw);
  }
  return out;
}

std::string plain_text(const Spans& sp) {
  std::string t;
  for (const Span& s : sp) t += s.text;
  return t;
}

Line make_line(Spans sp, bool highlight = false) {
  Line l;
  l.text = plain_text(sp);
  l.spans = std::move(sp);
  l.highlight = highlight;
  return l;
}

std::string repeat(const char* s, std::size_t n) {
  std::string out;
  for (std::size_t i = 0; i < n; ++i) out += s;
  return out;
}

// │ content … │ with the content clipped or padded to the inner width; `right` replaces the right border (scroll bar).
Line boxed(Spans content, std::size_t w, bool highlight = false, const Span* right = nullptr) {
  const std::size_t inner = w - 2;
  content = clip(content, inner);
  const std::size_t have = spans_width(content);
  Spans sp{{kV, Style::frame}};
  for (Span& c : content) sp.push_back(std::move(c));
  if (have < inner) sp.push_back({std::string(inner - have, ' '), Style::plain});
  sp.push_back(right ? *right : Span{kV, Style::frame});
  return make_line(std::move(sp), highlight);
}

// ┌─ left ──────── right ─┐ : the corners are `l` and `r`; the right text is dropped when there is no room.
Line border(const char* l, const char* r, Spans left, Spans right, std::size_t w) {
  const std::size_t avail = w - 2;
  auto padded = [](Spans sp) {
    if (sp.empty()) return sp;
    sp.insert(sp.begin(), Span{" ", Style::plain});
    sp.push_back({" ", Style::plain});
    return sp;
  };
  left = padded(std::move(left));
  right = right.empty() ? Spans() : padded(std::move(right));
  if (spans_width(left) + spans_width(right) + 2 > avail) right.clear();
  left = clip(left, avail > 2 ? avail - 2 : 0);
  const std::size_t used = spans_width(left) + spans_width(right);
  Spans sp{{l, Style::frame}, {kH, Style::frame}};
  for (Span& c : left) sp.push_back(std::move(c));
  sp.push_back({repeat(kH, avail > used + 2 ? avail - used - 2 : 0), Style::frame});
  for (Span& c : right) sp.push_back(std::move(c));
  sp.push_back({kH, Style::frame});
  sp.push_back({r, Style::frame});
  return make_line(std::move(sp));
}

Style scope_style(Scope s) {
  switch (s) {
    case Scope::global: return Style::global;
    case Scope::project: return Style::project;
    case Scope::file: return Style::file;
    case Scope::run: break;
  }
  return Style::run;
}

Style tag_style(Scope s) {
  switch (s) {
    case Scope::global: return Style::tag_global;
    case Scope::project: return Style::tag_project;
    case Scope::file: return Style::tag_file;
    case Scope::run: break;
  }
  return Style::tag_run;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// The home directory as ~, so a long path still shows what matters.
std::string short_path(const std::string& p) {
  const char* home = std::getenv("HOME");
  if (home && *home && p.rfind(home, 0) == 0 && (p.size() == std::strlen(home) || p[std::strlen(home)] == '/'))
    return "~" + p.substr(std::strlen(home));
  return p;
}

}  // namespace

std::vector<Line> Model::render(int rows, int cols) const {
  const std::size_t w = static_cast<std::size_t>(std::max(cols, 20));
  const std::size_t h = static_cast<std::size_t>(std::max(rows, 1));
  const std::size_t inner = w - 2;
  const std::vector<std::size_t> vis = visible();
  const std::size_t sel = std::min(sel_, vis.empty() ? 0 : vis.size() - 1);

  // Top border, scope bar, list rule, [list], help rule, [help], command rule, [command], status, keys, bottom border.
  const std::size_t help_n = h >= 34 ? 5 : (h >= 22 ? 3 : 2), cmd_n = h >= 34 ? 3 : (h >= 22 ? 2 : 1);
  const std::size_t fixed = 8 + help_n + cmd_n;
  const std::size_t body = h > fixed ? h - fixed : 1;
  if (sel < top_) top_ = sel;
  if (sel >= top_ + body) top_ = sel + 1 - body;
  if (top_ + body > vis.size()) top_ = vis.size() > body ? vis.size() - body : 0;

  std::size_t chosen = 0, hidden = 0, name_w = 0;
  for (const Field& f : fields_) chosen += f.value.empty() ? 0u : 1u, hidden += f.hidden ? 1u : 0u;
  for (std::size_t i : vis) name_w = std::max(name_w, width(fields_[i].label));
  name_w = std::min(name_w, inner / 2);
  constexpr std::size_t tag_w = 8;  // "this run" is the longest
  const std::size_t val_w = inner > name_w + tag_w + 7 ? inner - name_w - tag_w - 8 : 1;

  std::vector<Line> out;
  out.push_back(border("\xe2\x94\x8c", "\xe2\x94\x90", {{"tracemaker " + command_.substr(command_.find(' ') == std::string::npos ? 0 : command_.find(' ') + 1) + " options", Style::title}},
                       {{std::to_string(chosen) + " set \xc2\xb7 " + std::to_string(vis.size()) + " of " + std::to_string(fields_.size()) + " shown", Style::dim}}, w));

  // Scope bar: where an edit goes. Every scope that is available is a tab; the target one is lit.
  Spans bar{{" changes go to ", Style::dim}};
  for (int i = 1; i < kScopes; ++i) {
    if (!enabled_[i]) continue;
    const Scope sc = static_cast<Scope>(i);
    bar.push_back(sc == target_ ? Span{" " + upper(scope_name(sc)) + " ", tag_style(sc)} : Span{" " + upper(scope_name(sc)) + " ", Style::dim});
  }
  const int ti = static_cast<int>(target_);
  if (dirty_[ti]) bar.push_back({"  \xe2\x97\x8f unsaved", Style::warn});
  bar.push_back({exists_[ti] ? "  " : "  new: ", Style::dim});
  bar.push_back({short_path(path_[ti]), Style::label});
  out.push_back(boxed(bar, w));

  Spans list_title{{"Options", Style::heading}};
  if (mode_ == Mode::filter)
    list_title.push_back({"  filter: " + buffer_ + "_", Style::accent});
  else if (!filter_.empty())
    list_title.push_back({"  filter: " + filter_ + "  (Esc clears)", Style::accent});
  Spans list_right;
  if (!show_hidden_ && hidden > 0) list_right.push_back({std::to_string(hidden) + " experimental options hidden (h shows them)", Style::dim});
  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", list_title, list_right, w));

  // Scroll bar: the thumb replaces the right border on the rows it covers.
  const std::size_t thumb = vis.size() > body ? std::max<std::size_t>(1, body * body / vis.size()) : body;
  const std::size_t thumb_at = vis.size() > body ? (top_ * (body - thumb)) / (vis.size() - body) : 0;
  for (std::size_t r = 0; r < body; ++r) {
    const bool bar_here = vis.size() > body && r >= thumb_at && r < thumb_at + thumb;
    const Span right = bar_here ? Span{"\xe2\x94\x83", Style::accent} : Span{kV, Style::frame};
    if (top_ + r >= vis.size()) {
      out.push_back(boxed({{vis.empty() && r == 0 ? "  no option matches" : "", Style::dim}}, w, false, &right));
      continue;
    }
    const Field& f = fields_[vis[top_ + r]];
    const bool cur = top_ + r == sel;
    Spans sp{{cur ? " \xe2\x96\xb8 " : "   ", Style::accent}};
    sp.push_back({fit(f.label, name_w, true) + "  ", f.value.empty() ? Style::label : scope_style(f.scope)});
    std::string v;
    Style vs = f.value.empty() ? Style::dim : scope_style(f.scope);
    if (f.is_flag())
      v = f.value.empty() ? "[ ]" : (f.spellings.size() > 1 ? "[x] " + f.value : "[x]");
    else if (!f.value.empty())
      v = f.value;
    else
      v = f.def.empty() ? "-" : "(default " + f.def + ")";
    sp.push_back({fit(v, val_w, true) + "  ", vs});
    sp.push_back({fit(f.value.empty() ? "" : scope_name(f.scope), tag_w, true) + " ", f.value.empty() ? Style::dim : scope_style(f.scope)});
    out.push_back(boxed(std::move(sp), w, cur, &right));
  }

  // Help: the description of the option under the cursor, then where its value comes from.
  Spans help_title{{"Help", Style::heading}};
  if (!vis.empty()) help_title.push_back({"  " + fields_[vis[sel]].label, Style::label});
  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", help_title, {}, w));
  std::vector<Spans> help(help_n);
  if (!vis.empty()) {
    const Field& f = fields_[vis[sel]];
    const auto lines = wrap(f.help.empty() ? "(no description)" : f.help, inner - 2, help_n - 1);
    for (std::size_t i = 0; i + 1 < help_n; ++i) help[i] = {{" " + lines[i], Style::plain}};
    Spans meta{{" ", Style::plain}};
    auto item = [&](const std::string& k, const std::string& val, Style st = Style::plain) {
      if (meta.size() > 1) meta.push_back({"  \xc2\xb7  ", Style::dim});
      meta.push_back({k + " ", Style::dim});
      meta.push_back({val, st});
    };
    item(f.is_flag() ? "flag, Space cycles" : (f.arg.empty() ? "argument" : "value, Enter edits"), "");
    if (f.save) item("default", f.def.empty() ? (f.is_flag() ? "off" : "none") : f.def);
    if (f.value.empty()) {
      item("now", "default");
    } else {
      item("now", f.value, scope_style(f.scope));
      meta.push_back({std::string(" (") + scope_name(f.scope) + ")", scope_style(f.scope)});
    }
    if (f.save && !f.key.empty()) item("key", f.key);
    if (f.save && !f.value.empty() && f.scope != target_ && f.layer[static_cast<int>(target_)].empty() &&
        static_cast<int>(f.scope) > static_cast<int>(target_))
      item("", std::string("overrides ") + scope_name(target_), Style::warn);
    help[help_n - 1] = std::move(meta);
  } else {
    help[0] = {{" no option is selected", Style::dim}};
  }
  for (Spans& sp : help) out.push_back(boxed(std::move(sp), w));

  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", {{"Command", Style::heading}}, {}, w));
  for (const std::string& l : wrap("$ " + command_line(), inner - 2, cmd_n)) out.push_back(boxed({{" " + l, Style::code}}, w));

  // Status: a prompt while editing, else the last message, else the parser's verdict.
  Spans status;
  if (mode_ == Mode::edit && !vis.empty())
    status = {{" " + fields_[vis[sel]].label + " = ", Style::label}, {buffer_ + "_", Style::accent}, {"   Enter keeps, Esc cancels, empty = default", Style::dim}};
  else if (mode_ == Mode::save)
    status = {{std::string(" save ") + scope_name(target_) + " options to: ", Style::label}, {buffer_ + "_", Style::accent}};
  else if (!status_.empty())
    status = {{" " + status_, status_.rfind("cannot", 0) == 0 ? Style::error : Style::warn}};
  else if (!error_.empty())
    status = {{" not runnable: " + error_, Style::error}};
  else
    status = {{" ready to run", Style::ok}};
  out.push_back(boxed(std::move(status), w));

  static const std::pair<const char*, const char*> keys[] = {{"Space", "edit"}, {"d", "reset"}, {"Tab", "scope"}, {"/", "find"},
                                                            {"h", "more"},      {"s", "save"},    {"p", "print"},   {"r", "run"}, {"q", "quit"}};
  auto hints = [&](const char* gap) {
    Spans sp{{" ", Style::plain}};
    for (const auto& [k, d] : keys) {
      if (sp.size() > 1) sp.push_back({gap, Style::plain});
      sp.push_back({k, Style::key});
      sp.push_back({std::string(" ") + d, Style::dim});
    }
    return sp;
  };
  Spans hint = hints("  ");
  if (spans_width(hint) > inner) hint = hints(" ");
  out.push_back(boxed(std::move(hint), w));
  out.push_back(border("\xe2\x94\x94", "\xe2\x94\x98", {}, {}, w));
  out.resize(h);
  return out;
}

void save_options(Model& m) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path path(m.save_path());
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path);
  f << m.config_text();
  f.close();
  if (f)
    m.mark_saved();
  m.set_status(f ? std::string("saved ") + scope_name(m.target()) + " options to " + m.save_path()
                 : "cannot write " + m.save_path() + ": " + std::strerror(errno));
}

Action run_keys(Model& m, const std::string& keys) {
  static const std::pair<const char*, int> names[] = {
      {"up", key::up},       {"down", key::down}, {"pgup", key::page_up}, {"pgdn", key::page_down},
      {"home", key::home},   {"end", key::end},   {"enter", key::enter},  {"space", ' '},
      {"esc", key::esc},     {"bs", key::backspace}, {"del", key::del},   {"lt", '<'},
      {"tab", key::tab},     {"stab", key::shift_tab}};
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
