// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Lossless s-expression documents for KiCad files (design doc 08 §3).
//
// The parser keeps the original text and records each node's byte span, so writing an unmodified
// document reproduces the input byte for byte. Edits are recorded as span replacements and insertions
// and applied on write; untouched text is never re-formatted.
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tmk::sexpr {

using NodeId = std::uint32_t;
inline constexpr NodeId kNoNode = 0xFFFFFFFFu;

enum class Kind : std::uint8_t { List, Symbol, String };

struct Node {
  Kind kind;
  std::uint32_t begin;        // byte offset of '(' / first char of the atom / opening quote
  std::uint32_t end;          // one past ')' / last char / closing quote
  std::uint32_t first_child;  // index into Document::kids_ (lists only)
  std::uint32_t child_count;
  NodeId parent;
};

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& msg, std::size_t offset, int line)
      : std::runtime_error(msg + " at line " + std::to_string(line)), offset_(offset), line_(line) {}
  std::size_t offset() const { return offset_; }
  int line() const { return line_; }

 private:
  std::size_t offset_;
  int line_;
};

class Document {
 public:
  // Parses `text`. Throws ParseError on malformed input. The document owns a copy of the text.
  static Document parse(std::string text);
  static Document load(const std::string& path);

  NodeId root() const { return root_; }
  const Node& node(NodeId id) const { return nodes_[id]; }
  std::size_t node_count() const { return nodes_.size(); }
  const std::string& text() const { return text_; }

  // ---- navigation ----
  std::span<const NodeId> children(NodeId list) const {
    const Node& n = nodes_[list];
    return {kids_.data() + n.first_child, n.child_count};
  }
  NodeId child(NodeId list, std::size_t i) const {
    const Node& n = nodes_[list];
    return i < n.child_count ? kids_[n.first_child + i] : kNoNode;
  }
  // First token of a list, e.g. "segment" for (segment ...). Empty for atoms or empty lists.
  std::string_view head(NodeId list) const;
  bool is_list(NodeId id) const { return nodes_[id].kind == Kind::List; }
  // First child list whose head is `name`, or kNoNode.
  NodeId find(NodeId list, std::string_view name) const;
  // All child lists whose head is `name`.
  std::vector<NodeId> find_all(NodeId list, std::string_view name) const;

  // ---- atoms ----
  // Raw source text of a node (for strings, including quotes).
  std::string_view raw(NodeId id) const {
    const Node& n = nodes_[id];
    return std::string_view(text_).substr(n.begin, n.end - n.begin);
  }
  // Atom value: symbols as-is, strings unescaped.
  std::string str(NodeId atom) const;
  // Value of the i-th child atom of `list` (i counts the head as 0).
  std::string str_at(NodeId list, std::size_t i) const;
  std::optional<double> number_at(NodeId list, std::size_t i) const;
  // Parses a decimal millimetre value to integer nanometres exactly (no binary floating point).
  std::optional<std::int64_t> nm_at(NodeId list, std::size_t i) const;
  // 1-based line number of a byte offset (for error messages).
  int line_of(std::size_t offset) const;

  // ---- edits (applied by write()) ----
  // Replaces the node's text.
  void replace(NodeId id, std::string new_text);
  // Removes the node together with its leading whitespace on the same line and the line break, when the
  // node occupies whole lines; otherwise removes the node and one preceding space.
  void remove(NodeId id);
  // Inserts `text` as new children at the end of `list`, each on its own line, indented one level deeper
  // than the list's own line. `text` must be one or more complete s-expressions without leading indent;
  // embedded newlines are re-indented.
  void append_child(NodeId list, std::string_view text);
  bool modified() const { return !edits_.empty(); }
  // Text after applying all edits.
  std::string write() const;
  void save(const std::string& path) const;

  // The indentation (tabs/spaces) at the start of the line containing `offset`.
  std::string_view indent_of(std::size_t offset) const;

 private:
  struct Edit {
    std::uint32_t begin, end;  // replaced span [begin, end); begin == end for pure insertions
    std::string text;
    std::uint64_t seq;         // insertion order, for stable ordering of insertions at the same offset
  };

  std::string text_;
  std::vector<Node> nodes_;
  std::vector<NodeId> kids_;
  NodeId root_ = kNoNode;
  std::vector<Edit> edits_;
  mutable std::vector<std::uint32_t> line_starts_;
};

// Escapes a string for KiCad s-expressions and adds quotes.
std::string quote(std::string_view s);

// Formats nanometres as a KiCad millimetre number: shortest exact decimal, no trailing zeros ("0.25", "-12", "1.000001").
std::string format_mm(std::int64_t nm);

// Parses a decimal millimetre string to nanometres exactly; rounds half away from zero beyond 6 decimals.
std::optional<std::int64_t> parse_mm(std::string_view s);

}  // namespace tmk::sexpr
