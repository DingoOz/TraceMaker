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

}  // namespace tmk::io
