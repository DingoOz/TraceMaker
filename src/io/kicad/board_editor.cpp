#include "io/kicad/board_editor.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

#include "core/rng.hpp"

namespace tmk::io {

using sexpr::format_mm;
using sexpr::kNoNode;
using sexpr::quote;

std::string format_angle(double deg) {
  deg = geom::norm_deg(deg);
  const double r = std::round(deg * 1e6) / 1e6;  // KiCad keeps angles to 1e-6 degree in files
  char buf[32];
  const auto res = std::to_chars(buf, buf + sizeof buf, r);
  return std::string(buf, res.ptr);
}

std::string BoardEditor::next_uuid() {
  const RngStream s(seed_, 0xED17u, 0);
  const U32x4 b = s.block(uuid_counter_++);
  char out[40];
  std::snprintf(out, sizeof out, "%08x-%04x-4%03x-%04x-%04x%08x", b.v[0], b.v[1] >> 16, b.v[1] & 0x0FFFu,
                (b.v[2] >> 16 & 0x3FFFu) | 0x8000u, b.v[2] & 0xFFFFu, b.v[3]);
  return out;
}

std::string BoardEditor::net_expr(model::NetId net) const {
  const auto& b = lb_.board;
  const auto& n = b.nets[static_cast<std::size_t>(net)];
  if (b.named_nets) return net == 0 ? std::string() : "(net " + quote(n.name) + ")";
  return "(net " + std::to_string(n.file_number < 0 ? 0 : n.file_number) + ")";
}

void BoardEditor::add_track(const model::Track& t) {
  const auto& b = lb_.board;
  std::string s = "(segment\n";
  s += "\t(start " + format_mm(t.a.x) + " " + format_mm(t.a.y) + ")\n";
  s += "\t(end " + format_mm(t.b.x) + " " + format_mm(t.b.y) + ")\n";
  s += "\t(width " + format_mm(t.width) + ")\n";
  if (t.locked) s += "\t(locked yes)\n";
  s += "\t(layer " + quote(b.copper_file_name(t.layer)) + ")\n";
  if (const std::string n = net_expr(t.net); !n.empty()) s += "\t" + n + "\n";
  s += "\t(uuid " + quote(next_uuid()) + ")\n)";
  lb_.doc.append_child(lb_.doc.root(), s);
}

void BoardEditor::add_via(const model::Via& v) {
  const auto& b = lb_.board;
  std::string s = "(via";
  if (v.type == model::ViaType::Blind) s += " blind";
  if (v.type == model::ViaType::Micro) s += " micro";
  s += "\n\t(at " + format_mm(v.pos.x) + " " + format_mm(v.pos.y) + ")\n";
  s += "\t(size " + format_mm(v.size) + ")\n";
  s += "\t(drill " + format_mm(v.drill) + ")\n";
  if (v.locked) s += "\t(locked yes)\n";
  s += "\t(layers " + quote(b.copper_file_name(v.layer_top)) + " " + quote(b.copper_file_name(v.layer_bottom)) + ")\n";
  if (const std::string n = net_expr(v.net); !n.empty()) s += "\t" + n + "\n";
  s += "\t(uuid " + quote(next_uuid()) + ")\n)";
  lb_.doc.append_child(lb_.doc.root(), s);
}

void BoardEditor::remove_track(std::size_t index) { lb_.doc.remove(lb_.board.tracks.at(index).node); }
void BoardEditor::remove_via(std::size_t index) { lb_.doc.remove(lb_.board.vias.at(index).node); }

void BoardEditor::move_footprint(std::size_t index, model::Point pos, double angle) {
  auto& doc = lb_.doc;
  const auto& fp = lb_.board.footprints.at(index);
  const double delta = angle - fp.angle;
  const std::string a = format_angle(angle);
  const sexpr::NodeId at = doc.find(fp.node, "at");
  std::string at_text = "(at " + format_mm(pos.x) + " " + format_mm(pos.y) + (a == "0" ? "" : " " + a) + ")";
  if (at != kNoNode) doc.replace(at, at_text);
  else doc.append_child(fp.node, at_text);
  if (geom::norm_deg(delta) == 0.0) return;

  // KiCad stores pad and text orientations as absolute angles: rotate them with the footprint.
  auto rotate_at = [&](sexpr::NodeId item) {
    const sexpr::NodeId iat = doc.find(item, "at");
    if (iat == kNoNode) return;
    const auto x = doc.nm_at(iat, 1), y = doc.nm_at(iat, 2);
    if (!x || !y) return;
    const double old = doc.number_at(iat, 3).value_or(0.0);
    const std::string na = format_angle(old + delta);
    std::string t = "(at " + format_mm(*x) + " " + format_mm(*y) + (na == "0" && doc.child(iat, 3) == kNoNode ? "" : " " + na);
    // Keep trailing flags such as "unlocked".
    for (std::size_t i = 4; i < doc.children(iat).size(); ++i) t += " " + std::string(doc.raw(doc.child(iat, i)));
    t += ")";
    doc.replace(iat, t);
  };
  for (sexpr::NodeId c : doc.children(fp.node)) {
    if (!doc.is_list(c)) continue;
    const std::string_view h = doc.head(c);
    if (h == "pad" || h == "property" || h == "fp_text") rotate_at(c);
  }
}

// ---- flipping (KiCad FOOTPRINT::Flip, FLIP_DIRECTION::TOP_BOTTOM about the footprint origin) ----
//
// KiCad mirrors every child about the footprint's x axis in absolute coordinates and negates the footprint
// orientation. With M = diag(1, −1) and R(θ) the rotation, M·R(θ)·l = R(−θ)·M·l: in the file, where child
// positions are stored in the footprint's unrotated frame, that is exactly "negate local y, negate the footprint
// angle". Pad orientations (absolute in the file) become −a; text angles become 180° − a (PCB_TEXT::Flip) and the
// text is mirrored; side-specific layers swap. A rotation by `delta` after the flip then adds delta to every
// absolute angle, as move_footprint does. Verified against pcbnew's own Flip (scripts/flip_check.py).

std::string flip_layer_name(const model::Board& b, std::string_view n) {
  // Copper through the board's layer table: KiCad 5 files name copper layers by their user names ("Front", "Back").
  if (const int ci = b.copper_index(n); ci >= 0) {
    const int nc = b.copper_count();
    return ci == 0 ? b.copper_file_name(nc - 1) : ci == nc - 1 ? b.copper_file_name(0) : std::string(n);
  }
  static constexpr std::string_view kSided[] = {"Cu", "Adhes", "Paste", "SilkS", "Mask", "CrtYd", "Fab"};
  if (n.size() > 2 && (n.starts_with("F.") || n.starts_with("B.")))
    for (std::string_view s : kSided)
      if (n.substr(2) == s) return std::string(n[0] == 'F' ? "B." : "F.") + std::string(s);
  return std::string(n);
}

namespace {

using sexpr::Document;
using sexpr::NodeId;

// Heads of footprint children that carry no geometry (left untouched by a flip).
bool inert_child(std::string_view h) {
  static constexpr std::string_view k[] = {"tedit", "tstamp", "uuid", "path", "sheetname", "sheetfile", "descr", "tags", "attr", "model",
                                           "locked", "placed", "solder_mask_margin", "solder_paste_margin", "solder_paste_ratio",
                                           "solder_paste_margin_ratio", "clearance", "zone_connect", "thermal_width", "thermal_gap",
                                           "autoplace_cost90", "autoplace_cost180", "net_tie_pad_groups", "embedded_fonts", "embedded_files",
                                           "duplicate_pad_numbers_are_jumpers", "jumper_pad_groups", "component_classes", "version",
                                           "generator", "generator_version", "units"};
  for (std::string_view s : k)
    if (h == s) return true;
  return false;
}

bool graphic_child(std::string_view h) {
  return h == "fp_line" || h == "fp_rect" || h == "fp_circle" || h == "fp_arc" || h == "fp_poly" || h == "fp_curve";
}

bool inner_copper(const model::Board& b, std::string_view n) {
  const int ci = b.copper_index(n);
  return ci > 0 && ci < b.copper_count() - 1;
}

// The edits of one flip: the board for layer names, the document for the nodes.
struct Flipper {
  const model::Board& b;
  Document& doc;

  // Replaces a layer-name atom by its flipped name, keeping the quoting style.
  void atom(NodeId a) {
    if (doc.is_list(a)) return;
    const std::string v = doc.str(a);
    const std::string f = flip_layer_name(b, v);
    if (f == v) return;
    doc.replace(a, doc.raw(a).starts_with('"') ? quote(f) : f);
  }
  void layers(NodeId list) {
    const auto kids = doc.children(list);
    for (std::size_t i = 1; i < kids.size(); ++i) atom(kids[i]);
  }
  // Negates the y of every point inside `g` (start/end/mid/center/xy, nested in pts, arc and primitives), negates
  // legacy arc sweeps and flips layer names.
  void mirror(NodeId g) {
    for (NodeId c : doc.children(g)) {
      if (!doc.is_list(c)) continue;
      const std::string_view h = doc.head(c);
      if (h == "start" || h == "end" || h == "mid" || h == "center" || h == "xy") {
        const auto x = doc.nm_at(c, 1), y = doc.nm_at(c, 2);
        if (x && y) doc.replace(c, "(" + std::string(h) + " " + format_mm(*x) + " " + format_mm(-*y) + ")");
      } else if (h == "angle") {  // KiCad <= 5 arc: (start CENTRE) (end START) (angle SWEEP); a signed sweep, never normalised
        const NodeId v = doc.child(c, 1);
        if (v != kNoNode && !doc.is_list(v) && doc.number_at(c, 1)) {
          const std::string_view s = doc.raw(v);
          doc.replace(v, s.starts_with('-') ? std::string(s.substr(1)) : s.starts_with('+') ? "-" + std::string(s.substr(1)) : "-" + std::string(s));
        }
      } else if (h == "layer" || h == "layers") {
        layers(c);
      } else if (h == "pts" || h == "arc" || h == "primitives" || h.starts_with("gr_")) {
        mirror(c);
      }
    }
  }
};

// (at x y [a] [flags...]) → (at x −y a' [flags...]); `new_angle` maps the old absolute angle.
template <typename F>
void flip_at(Document& doc, NodeId item, F new_angle) {
  const NodeId at = doc.find(item, "at");
  if (at == kNoNode) return;
  const auto x = doc.nm_at(at, 1), y = doc.nm_at(at, 2);
  if (!x || !y) return;
  const auto old = doc.number_at(at, 3);
  const std::string na = format_angle(new_angle(old.value_or(0.0)));
  std::string t = "(at " + format_mm(*x) + " " + format_mm(-*y) + (na == "0" && !old ? "" : " " + na);
  for (std::size_t i = old ? 4 : 3; i < doc.children(at).size(); ++i) t += " " + std::string(doc.raw(doc.child(at, i)));
  doc.replace(at, t + ")");
}

// Mirrored text: toggle "mirror" in (effects ... (justify ...)).
void toggle_mirror(Document& doc, NodeId text) {
  const NodeId e = doc.find(text, "effects");
  if (e == kNoNode) {
    doc.append_child(text, "(effects (justify mirror))");
    return;
  }
  const NodeId j = doc.find(e, "justify");
  if (j == kNoNode) {
    const std::string_view raw = doc.raw(e);
    doc.replace(e, std::string(raw.substr(0, raw.size() - 1)) + " (justify mirror))");
    return;
  }
  std::string keep;
  bool had = false;
  const auto kids = doc.children(j);
  for (std::size_t i = 1; i < kids.size(); ++i) {
    if (doc.raw(kids[i]) == "mirror") {
      had = true;
      continue;
    }
    keep += " " + std::string(doc.raw(kids[i]));
  }
  if (!had) keep += " mirror";
  if (keep.empty()) doc.remove(j);
  else doc.replace(j, "(justify" + keep + ")");
}

}  // namespace

bool flip_supported(const LoadedBoard& lb, std::size_t index, std::string* why) {
  const auto& doc = lb.doc;
  const auto& fp = lb.board.footprints.at(index);
  auto no = [&](std::string r) {
    if (why) *why = std::move(r);
    return false;
  };
  // Copper on inner layers would need KiCad's stack-aware FlipLayer; such footprints are through-hole anyway.
  auto inner_in = [&](NodeId list) {
    for (NodeId c : doc.children(list))
      if (!doc.is_list(c) && inner_copper(lb.board, doc.str(c))) return true;
    return false;
  };
  for (NodeId c : doc.children(fp.node)) {
    if (!doc.is_list(c)) continue;
    const std::string_view h = doc.head(c);
    if (h == "layer" || h == "at" || inert_child(h)) continue;
    if (h == "pad") {
      if (doc.find(c, "padstack") != kNoNode) return no("pad with a padstack");
      if (doc.find(c, "zone_layer_connections") != kNoNode) return no("pad with zone layer connections");
      if (const NodeId l = doc.find(c, "layers"); l != kNoNode && inner_in(l)) return no("pad on an inner layer");
      continue;
    }
    if (h == "property" || h == "fp_text") {
      if (const NodeId l = doc.find(c, "layer"); l != kNoNode && lb.board.copper_index(doc.str_at(l, 1)) >= 0) return no("text on copper");
      continue;
    }
    if (graphic_child(h)) {
      if (const NodeId l = doc.find(c, "layer"); l != kNoNode && inner_copper(lb.board, doc.str_at(l, 1))) return no("graphic on an inner layer");
      continue;
    }
    return no("footprint item '" + std::string(h) + "'");
  }
  return true;
}

void BoardEditor::flip_footprint(std::size_t index, model::Point pos, double angle) {
  auto& doc = lb_.doc;
  const auto& fp = lb_.board.footprints.at(index);
  const double delta = angle + fp.angle;  // the flip alone gives −angle0; then rotate by delta
  Flipper fl{lb_.board, doc};
  const std::string a = format_angle(angle);
  const NodeId at = doc.find(fp.node, "at");
  std::string at_text = "(at " + format_mm(pos.x) + " " + format_mm(pos.y) + (a == "0" ? "" : " " + a) + ")";
  if (at != kNoNode) doc.replace(at, at_text);
  else doc.append_child(fp.node, at_text);
  for (NodeId c : doc.children(fp.node)) {
    if (!doc.is_list(c)) continue;
    const std::string_view h = doc.head(c);
    if (h == "layer") {
      fl.layers(c);
    } else if (h == "pad") {
      flip_at(doc, c, [&](double old) { return -old + delta; });
      for (NodeId q : doc.children(c)) {
        if (!doc.is_list(q)) continue;
        const std::string_view ph = doc.head(q);
        if (ph == "layers") {
          fl.layers(q);
        } else if (ph == "rect_delta") {
          // Trapezoid deltas live in the pad frame. KiCad's corners are (∓(hx + dy), ±(hy ± dx)) with d = delta/2;
          // mirroring y maps that set onto itself with dy negated and dx kept (KiCad PAD::Flip: MIRROR(m_deltaSize.y)).
          const auto dx = doc.nm_at(q, 1), dy = doc.nm_at(q, 2);
          if (dx && dy) doc.replace(q, "(rect_delta " + format_mm(*dx) + " " + format_mm(-*dy) + ")");
        } else if (ph == "chamfer") {
          std::string t = "(chamfer";
          const auto kids = doc.children(q);
          for (std::size_t i = 1; i < kids.size(); ++i) {
            std::string v(doc.raw(kids[i]));
            if (v.starts_with("top_")) v = "bottom_" + v.substr(4);
            else if (v.starts_with("bottom_")) v = "top_" + v.substr(7);
            t += " " + v;
          }
          doc.replace(q, t + ")");
        } else if (ph == "drill") {
          if (const NodeId off = doc.find(q, "offset"); off != kNoNode) {
            const auto x = doc.nm_at(off, 1), y = doc.nm_at(off, 2);
            if (x && y) doc.replace(off, "(offset " + format_mm(*x) + " " + format_mm(-*y) + ")");
          }
        } else if (ph == "primitives") {
          fl.mirror(q);
        }
      }
    } else if (h == "property" || h == "fp_text") {
      const NodeId l = doc.find(c, "layer");
      if (l == kNoNode) continue;  // KiCad 6/7 non-visual property ("Sheetfile", ...)
      flip_at(doc, c, [&](double old) { return 180.0 - old + delta; });
      // KiCad mirrors only text on side-specific layers (PCB_TEXT::Flip: IsSideSpecific()).
      if (const std::string ln = doc.str_at(l, 1); flip_layer_name(lb_.board, ln) != ln) toggle_mirror(doc, c);
      fl.layers(l);
    } else if (graphic_child(h)) {
      fl.mirror(c);
    }
  }
}

}  // namespace tmk::io
