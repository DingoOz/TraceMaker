#include "sexpr/sexpr.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tmk::sexpr {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_delim(char c) { return is_space(c) || c == '(' || c == ')' || c == '"'; }

}  // namespace

Document Document::parse(std::string text) {
  Document d;
  d.text_ = std::move(text);
  const std::string& t = d.text_;
  if (t.size() >= 0xFFFFFFF0u) throw ParseError("file too large", 0, 1);
  d.nodes_.reserve(t.size() / 6);
  d.kids_.reserve(t.size() / 6);

  // Each open list keeps its pending children on a shared stack; on ')' they move to kids_.
  std::vector<NodeId> open;          // open list node ids
  std::vector<NodeId> pending;       // children of all open lists, in order
  std::vector<std::size_t> marks;    // pending.size() when each open list started

  auto add_atom = [&](Kind k, std::size_t b, std::size_t e) {
    if (open.empty()) throw ParseError("atom outside a list", b, d.line_of(b));
    const NodeId id = static_cast<NodeId>(d.nodes_.size());
    d.nodes_.push_back(Node{k, static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(e), 0, 0, open.back()});
    pending.push_back(id);
  };

  std::size_t i = 0;
  const std::size_t n = t.size();
  while (i < n) {
    const char c = t[i];
    if (is_space(c)) {
      ++i;
    } else if (c == '(') {
      if (open.empty() && d.root_ != kNoNode) throw ParseError("more than one top-level expression", i, d.line_of(i));
      const NodeId id = static_cast<NodeId>(d.nodes_.size());
      d.nodes_.push_back(Node{Kind::List, static_cast<std::uint32_t>(i), 0, 0, 0, open.empty() ? kNoNode : open.back()});
      if (!open.empty()) pending.push_back(id);
      else d.root_ = id;
      open.push_back(id);
      marks.push_back(pending.size());
      ++i;
    } else if (c == ')') {
      if (open.empty()) throw ParseError("unbalanced ')'", i, d.line_of(i));
      const NodeId id = open.back();
      const std::size_t m = marks.back();
      Node& node = d.nodes_[id];
      node.end = static_cast<std::uint32_t>(i + 1);
      node.first_child = static_cast<std::uint32_t>(d.kids_.size());
      node.child_count = static_cast<std::uint32_t>(pending.size() - m);
      d.kids_.insert(d.kids_.end(), pending.begin() + static_cast<std::ptrdiff_t>(m), pending.end());
      pending.resize(m);
      open.pop_back();
      marks.pop_back();
      ++i;
    } else if (c == '"') {
      const std::size_t b = i++;
      while (i < n && t[i] != '"') {
        if (t[i] == '\\' && i + 1 < n) ++i;
        ++i;
      }
      if (i >= n) throw ParseError("unterminated string", b, d.line_of(b));
      ++i;
      add_atom(Kind::String, b, i);
    } else {
      const std::size_t b = i;
      while (i < n && !is_delim(t[i])) ++i;
      add_atom(Kind::Symbol, b, i);
    }
  }
  if (!open.empty()) throw ParseError("unclosed '('", d.nodes_[open.back()].begin, d.line_of(d.nodes_[open.back()].begin));
  if (d.root_ == kNoNode) throw ParseError("empty document", 0, 1);
  return d;
}

Document Document::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(std::move(ss).str());
}

std::string_view Document::head(NodeId list) const {
  const Node& n = nodes_[list];
  if (n.kind != Kind::List || n.child_count == 0) return {};
  const NodeId h = kids_[n.first_child];
  return nodes_[h].kind == Kind::Symbol ? raw(h) : std::string_view{};
}

NodeId Document::find(NodeId list, std::string_view name) const {
  for (NodeId c : children(list))
    if (nodes_[c].kind == Kind::List && head(c) == name) return c;
  return kNoNode;
}

std::vector<NodeId> Document::find_all(NodeId list, std::string_view name) const {
  std::vector<NodeId> out;
  for (NodeId c : children(list))
    if (nodes_[c].kind == Kind::List && head(c) == name) out.push_back(c);
  return out;
}

std::string Document::str(NodeId atom) const {
  const Node& n = nodes_[atom];
  const std::string_view r = raw(atom);
  if (n.kind != Kind::String) return std::string(r);
  std::string out;
  out.reserve(r.size());
  for (std::size_t i = 1; i + 1 < r.size(); ++i) {
    char c = r[i];
    if (c == '\\' && i + 2 < r.size()) {
      const char e = r[++i];
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        default: c = e; break;  // \" \\ and anything else: literal
      }
    }
    out.push_back(c);
  }
  return out;
}

std::string Document::str_at(NodeId list, std::size_t i) const {
  const NodeId c = child(list, i);
  return c == kNoNode || nodes_[c].kind == Kind::List ? std::string{} : str(c);
}

std::optional<double> Document::number_at(NodeId list, std::size_t i) const {
  const NodeId c = child(list, i);
  if (c == kNoNode || nodes_[c].kind != Kind::Symbol) return std::nullopt;
  const std::string_view r = raw(c);
  double v = 0;
  const auto res = std::from_chars(r.data(), r.data() + r.size(), v);
  if (res.ec != std::errc{} || res.ptr != r.data() + r.size()) return std::nullopt;
  return v;
}

std::optional<std::int64_t> Document::nm_at(NodeId list, std::size_t i) const {
  const NodeId c = child(list, i);
  if (c == kNoNode || nodes_[c].kind != Kind::Symbol) return std::nullopt;
  return parse_mm(raw(c));
}

int Document::line_of(std::size_t offset) const {
  if (line_starts_.empty()) {
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i)
      if (text_[i] == '\n') line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
  }
  const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
  return static_cast<int>(it - line_starts_.begin());
}

std::string_view Document::indent_of(std::size_t offset) const {
  std::size_t ls = offset;
  while (ls > 0 && text_[ls - 1] != '\n') --ls;
  std::size_t e = ls;
  while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t')) ++e;
  return std::string_view(text_).substr(ls, e - ls);
}

void Document::replace(NodeId id, std::string new_text) {
  const Node& n = nodes_[id];
  // Replacing the same node again overrides the earlier replacement.
  for (auto& e : edits_)
    if (e.begin == n.begin && e.end == n.end && e.begin != e.end) {
      e.text = std::move(new_text);
      return;
    }
  edits_.push_back(Edit{n.begin, n.end, std::move(new_text), edits_.size()});
}

void Document::remove(NodeId id) {
  const Node& n = nodes_[id];
  std::size_t b = n.begin, e = n.end;
  std::size_t ls = b;
  while (ls > 0 && (text_[ls - 1] == ' ' || text_[ls - 1] == '\t')) --ls;
  std::size_t le = e;
  while (le < text_.size() && (text_[le] == ' ' || text_[le] == '\t')) ++le;
  const bool line_start = ls == 0 || text_[ls - 1] == '\n';
  const bool line_end = le == text_.size() || text_[le] == '\n' || text_[le] == '\r';
  if (line_start && line_end) {
    b = ls;
    e = le;
    if (e < text_.size() && text_[e] == '\r') ++e;
    if (e < text_.size() && text_[e] == '\n') ++e;
  } else if (b > 0 && text_[b - 1] == ' ') {
    --b;
  }
  edits_.push_back(Edit{static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(e), {}, edits_.size()});
}

void Document::append_child(NodeId list, std::string_view text) {
  const Node& n = nodes_[list];
  const std::size_t close = n.end - 1;  // position of ')'
  const std::string_view list_indent = indent_of(n.begin);
  const bool spaces = !list_indent.empty() && list_indent[0] == ' ';
  const std::string child_indent = std::string(list_indent) + (spaces ? "  " : "\t");

  // Re-indent every line of the inserted text.
  std::string body;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    const std::string_view line = text.substr(pos, nl - pos);
    if (!line.empty()) {
      body += child_indent;
      body += line;
      body += '\n';
    }
    pos = nl + 1;
  }

  // Is ')' the first non-blank character on its line? Then insert whole lines before that line.
  std::size_t ls = close;
  while (ls > 0 && (text_[ls - 1] == ' ' || text_[ls - 1] == '\t')) --ls;
  if (ls == 0 || text_[ls - 1] == '\n') {
    edits_.push_back(Edit{static_cast<std::uint32_t>(ls), static_cast<std::uint32_t>(ls), std::move(body), edits_.size()});
  } else {
    // Inline list: put children on new lines and the ')' on its own line.
    edits_.push_back(Edit{static_cast<std::uint32_t>(close), static_cast<std::uint32_t>(close),
                          "\n" + body + std::string(list_indent), edits_.size()});
  }
}

std::string Document::write() const {
  if (edits_.empty()) return text_;
  std::vector<const Edit*> order;
  order.reserve(edits_.size());
  for (const auto& e : edits_) order.push_back(&e);
  std::sort(order.begin(), order.end(), [](const Edit* a, const Edit* b) {
    if (a->begin != b->begin) return a->begin < b->begin;
    return a->seq < b->seq;
  });
  std::string out;
  out.reserve(text_.size() + 1024);
  std::size_t cur = 0;
  for (const Edit* e : order) {
    if (e->begin < cur) throw std::logic_error("overlapping s-expression edits");
    out.append(text_, cur, e->begin - cur);
    out += e->text;
    cur = e->end;
  }
  out.append(text_, cur, std::string::npos);
  return out;
}

void Document::save(const std::string& path) const {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  const std::string s = write();
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::string quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string format_mm(std::int64_t nm) {
  const bool neg = nm < 0;
  const std::uint64_t a = neg ? static_cast<std::uint64_t>(-(nm + 1)) + 1 : static_cast<std::uint64_t>(nm);
  std::uint64_t ip = a / 1'000'000, fp = a % 1'000'000;
  std::string out = neg ? "-" : "";
  out += std::to_string(ip);
  if (fp) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06llu", static_cast<unsigned long long>(fp));
    std::string f(buf);
    while (!f.empty() && f.back() == '0') f.pop_back();
    out += '.';
    out += f;
  }
  return out;
}

std::optional<std::int64_t> parse_mm(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') {
    neg = s[0] == '-';
    ++i;
  }
  std::int64_t ip = 0;
  bool digits = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ip = ip * 10 + (s[i] - '0');
    ++i;
    digits = true;
    if (ip > 9'000'000'000'000LL) return std::nullopt;
  }
  std::int64_t fp = 0;
  int fd = 0;
  bool round_up = false;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      if (fd < 6) {
        fp = fp * 10 + (s[i] - '0');
        ++fd;
      } else if (fd == 6) {
        round_up = s[i] >= '5';
        ++fd;
      }
      ++i;
      digits = true;
    }
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    // Exponent notation is not written by KiCad, but accept it through floating point.
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return static_cast<std::int64_t>(v * 1e6 + (v < 0 ? -0.5 : 0.5));
  }
  if (!digits || i != s.size()) return std::nullopt;
  for (int k = fd > 6 ? 6 : fd; k < 6; ++k) fp *= 10;
  std::int64_t v = ip * 1'000'000 + fp + (round_up ? 1 : 0);
  return neg ? -v : v;
}

}  // namespace tmk::sexpr
