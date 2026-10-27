// SPDX-License-Identifier: GPL-3.0-or-later
// Generated pours and the copper layer count (doc 05 §53): the two document edits behind `route --pour` and
// `route --try-layers`. Everything they do not name stays byte for byte (rule 8).
#include <algorithm>
#include <bit>

#include "io/kicad/board_editor.hpp"

namespace tmk::io {

using sexpr::format_mm;
using sexpr::kNoNode;
using sexpr::NodeId;
using sexpr::quote;

namespace {

// "(pts\n\t(xy a b) (xy c d) ...\n)", four points to a line as KiCad writes them.
std::string pts_text(const std::vector<model::Point>& ring) {
  std::string s = "(pts";
  for (std::size_t i = 0; i < ring.size(); ++i) {
    s += i % 4 == 0 ? "\n\t" : " ";
    s += "(xy " + format_mm(ring[i].x) + " " + format_mm(ring[i].y) + ")";
  }
  return s + "\n)";
}

std::string indent_lines(const std::string& text) {
  std::string out = "\t";
  for (char c : text) {
    out += c;
    if (c == '\n') out += '\t';
  }
  return out;
}

std::string filled_polygon_text(const model::Board& b, int layer, const std::vector<model::Point>& ring) {
  return "(filled_polygon\n\t(layer " + quote(b.copper_file_name(layer)) + ")\n" + indent_lines(pts_text(ring)) + "\n)";
}

}  // namespace

void BoardEditor::add_zone(const model::Zone& z) {
  const auto& b = lb_.board;
  const int layer = z.copper ? std::countr_zero(z.copper) : 0;
  std::string s = "(zone\n";
  if (const std::string n = net_expr(z.net); !n.empty()) s += "\t" + n + "\n";
  if (!b.named_nets) s += "\t(net_name " + quote(b.nets[static_cast<std::size_t>(z.net)].name) + ")\n";  // KiCad <= 9 zones name their net too
  s += "\t(layer " + quote(b.copper_file_name(layer)) + ")\n";
  s += "\t(uuid " + quote(z.uuid.empty() ? next_uuid() : z.uuid) + ")\n";
  if (!z.name.empty()) s += "\t(name " + quote(z.name) + ")\n";
  s += "\t(hatch edge 0.5)\n";
  if (z.priority > 0) s += "\t(priority " + std::to_string(z.priority) + ")\n";
  s += "\t(connect_pads" + std::string(z.connect == model::ZoneConnect::Full ? " yes" : z.connect == model::ZoneConnect::None ? " no" : "") +
       "\n\t\t(clearance " + format_mm(std::max<Coord>(z.clearance, 0)) + ")\n\t)\n";
  s += "\t(min_thickness " + format_mm(z.min_thickness) + ")\n";
  s += "\t(filled_areas_thickness no)\n";  // fills are the copper itself; KiCad 5 files would otherwise stroke them with min_thickness
  s += "\t(fill" + std::string(z.fills.empty() ? "" : " yes") + "\n\t\t(thermal_gap " + format_mm(z.thermal_gap) + ")\n\t\t(thermal_bridge_width " +
       format_mm(z.thermal_bridge_width) + ")\n\t\t(island_removal_mode " + std::to_string(z.island_removal) + ")\n\t)\n";
  s += "\t(polygon\n" + indent_lines(indent_lines(pts_text(z.outline.front()))) + "\n\t)\n";
  for (const auto& [l, ring] : z.fills) s += indent_lines(filled_polygon_text(b, l, ring)) + "\n";
  s += ")";
  lb_.doc.append_child(lb_.doc.root(), s);
}

void BoardEditor::set_zone_fills(std::size_t index, const std::vector<std::pair<int, std::vector<model::Point>>>& fills) {
  auto& doc = lb_.doc;
  const NodeId zn = lb_.board.zones.at(index).node;
  for (const NodeId f : doc.find_all(zn, "filled_polygon")) doc.remove(f);
  if (const NodeId f = doc.find(zn, "fill"); f != kNoNode) {
    const NodeId first = doc.child(f, 1);
    if (first != kNoNode && !doc.is_list(first)) {
      if (doc.str(first) != "yes") doc.replace(first, "yes");
    } else {
      // (fill (thermal_gap ...) ...) -> (fill yes (thermal_gap ...) ...), the rest of the node as written.
      doc.replace(f, "(fill yes" + std::string(doc.raw(f).substr(5)));
    }
  }
  for (const auto& [l, ring] : fills) doc.append_child(zn, filled_polygon_text(lb_.board, l, ring));
}

bool BoardEditor::set_copper_layer_count(int n, std::string* why, std::vector<std::string>* notes) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  const auto& b = lb_.board;
  auto& doc = lb_.doc;
  const int have = b.copper_count();
  if (n < 2 || n > 32 || n % 2 != 0) return fail("a copper layer count must be even, from 2 to 32");
  if (have < 2) return fail("the board has fewer than two copper layers");
  if (n == have) return true;
  const NodeId table = doc.find(doc.root(), "layers");
  if (table == kNoNode) return fail("the board has no layer table");
  // KiCad 9 numbering: F.Cu 0, B.Cu 2, In1 4, In2 6, ...; before: In1..In30 = 1..30, B.Cu 31. Taken from the file's own B.Cu entry
  // (hand-written and converted boards do not always match their version number).
  const bool v9 = b.layers[static_cast<std::size_t>(b.copper.back())].ordinal == 2;
  const NodeId front = [&] {
    for (const NodeId c : doc.children(table))
      if (doc.is_list(c) && doc.number_at(c, 0).value_or(-1) == 0) return c;
    return kNoNode;
  }();
  if (front == kNoNode) return fail("the layer table has no F.Cu entry");
  if (n > have) {
    // New entries go after the last inner layer (or F.Cu), in the file's own quoting.
    NodeId after = front;
    for (const NodeId c : doc.children(table)) {
      if (!doc.is_list(c)) continue;
      const int ord = static_cast<int>(doc.number_at(c, 0).value_or(-1));
      if (v9 ? ord >= 4 && ord % 2 == 0 && ord <= 2 * (have - 2) + 2 : ord >= 1 && ord <= have - 2) after = c;
    }
    const bool quoted = doc.raw(front).find('"') != std::string_view::npos;
    const std::string indent(doc.indent_of(doc.node(front).begin));
    std::string text(doc.raw(after));
    for (int k = have - 1; k <= n - 2; ++k) {
      const std::string name = "In" + std::to_string(k) + ".Cu";
      text += "\n" + indent + "(" + std::to_string(v9 ? 2 * k + 2 : k) + " " + (quoted ? quote(name) : name) + " signal)";
    }
    doc.replace(after, text);
  } else {
    // Removing In(n-1) .. In(have-2): nothing may be on them.
    model::LayerMask gone = 0;
    for (int l = n - 1; l <= have - 2; ++l) gone |= model::layer_bit(l);
    auto on_gone = [&](int l) { return l >= 0 && (gone & model::layer_bit(l)) != 0; };
    auto names_gone = [&](const std::vector<std::string>& names) {
      return std::any_of(names.begin(), names.end(), [&](const std::string& s) { return on_gone(b.copper_index(s)); });
    };
    for (const auto& t : b.tracks)
      if (on_gone(t.layer)) return fail("a track is on " + b.copper_name(t.layer));
    for (const auto& a : b.arcs)
      if (on_gone(a.layer)) return fail("an arc track is on " + b.copper_name(a.layer));
    for (const auto& v : b.vias)
      if (v.type != model::ViaType::Through && (on_gone(v.layer_top) || on_gone(v.layer_bottom)))
        return fail("a blind or micro via ends on an inner layer that would be removed");
    for (const auto& z : b.zones)
      if (z.copper & gone) return fail("zone " + (z.name.empty() ? std::string("(unnamed)") : z.name) + " is on an inner layer that would be removed");
    for (const auto& g : b.graphics)
      if (on_gone(b.copper_index(g.layer))) return fail("a graphic is on " + g.layer);
    for (const auto& t : b.texts)
      if (on_gone(b.copper_index(t.layer))) return fail("a text is on " + t.layer);
    for (const auto& p : b.pads)
      if (names_gone(p.layers)) return fail("a pad names an inner layer that would be removed");
    for (const NodeId c : doc.children(table)) {
      if (!doc.is_list(c)) continue;
      const int ord = static_cast<int>(doc.number_at(c, 0).value_or(-1));
      for (int l = n - 1; l <= have - 2; ++l)
        if (ord == (v9 ? 2 * l + 2 : l)) doc.remove(c);
    }
  }
  if (const NodeId setup = doc.find(doc.root(), "setup"); setup != kNoNode)
    if (const NodeId st = doc.find(setup, "stackup"); st != kNoNode) {
      doc.remove(st);
      if (notes) notes->push_back("the stackup block was removed: KiCad builds its default stackup for " + std::to_string(n) + " copper layers");
    }
  return true;
}

}  // namespace tmk::io
