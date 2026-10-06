// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/kicad/board_reader.hpp"

#include "geom/shape.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace tmk::io {
namespace {

using model::Board;
using model::LayerMask;
using model::NetId;
using sexpr::Document;
using sexpr::kNoNode;
using sexpr::NodeId;

class Reader {
 public:
  explicit Reader(const Document& d) : d_(d) {}

  Board run() {
    const NodeId root = d_.root();
    if (d_.head(root) != "kicad_pcb") throw std::runtime_error("not a kicad_pcb file");
    if (NodeId v = d_.find(root, "version"); v != kNoNode) b_.version = static_cast<std::int64_t>(d_.number_at(v, 1).value_or(0));
    if (NodeId g = d_.find(root, "generator"); g != kNoNode) b_.generator = d_.str_at(g, 1);
    if (NodeId g = d_.find(root, "general"); g != kNoNode)
      if (NodeId t = d_.find(g, "thickness"); t != kNoNode) b_.thickness = d_.nm_at(t, 1).value_or(b_.thickness);
    read_layers(d_.find(root, "layers"));
    if (NodeId setup = d_.find(root, "setup"); setup != kNoNode) {
      b_.pad_to_mask_clearance = child_nm(setup, "pad_to_mask_clearance", 0);
      if (NodeId t = d_.find(setup, "tenting"); t != kNoNode) b_.vias_tented = has_symbol(t, "front") || has_symbol(t, "back");
      if (NodeId st = d_.find(setup, "stackup"); st != kNoNode) read_stackup(st);
    }
    b_.nets.push_back(model::Net{0, "", 0});
    b_.net_index[""] = 0;
    const auto table = d_.find_all(root, "net");
    b_.named_nets = table.empty();
    for (NodeId n : table) {
      const int num = static_cast<int>(d_.number_at(n, 1).value_or(-1));
      const std::string name = d_.str_at(n, 2);
      if (num == 0) {
        b_.nets[0].file_number = 0;
        continue;
      }
      const NetId id = intern_net(name);
      b_.nets[static_cast<std::size_t>(id)].file_number = num;
      by_number_[num] = id;
    }
    for (NodeId c : d_.children(root)) {
      if (!d_.is_list(c)) continue;
      const std::string_view h = d_.head(c);
      if (h == "footprint" || h == "module") read_footprint(c);
      else if (h == "segment") read_segment(c);
      else if (h == "arc") read_arc(c);
      else if (h == "via") read_via(c);
      else if (h == "zone") read_zone(c, -1);
      else if (h.starts_with("gr_") && h != "gr_text" && h != "gr_text_box") read_graphic(c, -1, Point{}, 0, false);
      else if (h == "gr_text") read_text(c, -1, Point{}, 0);
    }
    return std::move(b_);
  }

 private:
  using Point = model::Point;

  // ---------- helpers ----------
  Point xy(NodeId n, std::size_t i = 1) const {
    return Point{d_.nm_at(n, i).value_or(0), d_.nm_at(n, i + 1).value_or(0)};
  }
  Point child_xy(NodeId n, std::string_view name) const {
    const NodeId c = d_.find(n, name);
    return c == kNoNode ? Point{} : xy(c);
  }
  Coord child_nm(NodeId n, std::string_view name, Coord def = 0) const {
    const NodeId c = d_.find(n, name);
    return c == kNoNode ? def : d_.nm_at(c, 1).value_or(def);
  }
  bool has_symbol(NodeId list, std::string_view sym) const {
    for (NodeId c : d_.children(list))
      if (!d_.is_list(c) && d_.node(c).kind == sexpr::Kind::Symbol && d_.raw(c) == sym) return true;
    return false;
  }
  bool yes(NodeId list, std::string_view name) const {
    // (name yes) / (name) / bare symbol `name`
    const NodeId c = d_.find(list, name);
    if (c != kNoNode) {
      const std::string v = d_.str_at(c, 1);
      return v.empty() || v == "yes" || v == "true";
    }
    return has_symbol(list, name);
  }

  // (stackup (layer "F.Cu" (type "copper") (thickness 0.035)) (layer "dielectric 1" (type "core") (thickness 0.1 locked)
  //  (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02) addsublayer (thickness 0.1) ...) ... (copper_finish "ENIG"))
  // Read-only: the block is never rewritten (rule 8). Copper layers are mapped to stack indices after the layer table.
  void read_stackup(NodeId st) {
    model::Stackup& s = b_.stackup;
    s.present = true;
    for (NodeId c : d_.children(st)) {
      if (!d_.is_list(c)) continue;
      const std::string_view h = d_.head(c);
      if (h == "copper_finish") s.copper_finish = d_.str_at(c, 1);
      else if (h == "dielectric_constraints") s.dielectric_constraints = d_.str_at(c, 1) == "yes";
      else if (h == "layer") s.layers.push_back(read_stackup_layer(c));
    }
  }

  model::StackupLayer read_stackup_layer(NodeId n) {
    model::StackupLayer l;
    l.name = d_.str_at(n, 1);
    if (NodeId t = d_.find(n, "type"); t != kNoNode) l.type = d_.str_at(t, 1);
    l.copper_index = l.type == "copper" ? b_.copper_index(l.name) : -1;
    if (l.type == "copper" && l.copper_index < 0)
      b_.warnings.push_back("stackup copper layer " + l.name + " is not in the layer table");
    // Sublayers: the children after each bare `addsublayer` symbol describe one more dielectric sheet.
    struct Sheet { Coord h = 0; double er = 0, tand = 0; bool has_er = false; std::string material; };
    std::vector<Sheet> sheets(1);
    for (NodeId c : d_.children(n)) {
      if (!d_.is_list(c)) {
        if (d_.node(c).kind == sexpr::Kind::Symbol && d_.raw(c) == "addsublayer") sheets.emplace_back();
        continue;
      }
      const std::string_view h = d_.head(c);
      Sheet& sh = sheets.back();
      if (h == "thickness") sh.h = d_.nm_at(c, 1).value_or(0);
      else if (h == "epsilon_r") {
        if (const auto v = d_.number_at(c, 1); v && *v > 0) {
          sh.er = *v;
          sh.has_er = true;
        }
      } else if (h == "loss_tangent") sh.tand = d_.number_at(c, 1).value_or(0);
      else if (h == "material") sh.material = d_.str_at(c, 1);
    }
    l.sublayers = static_cast<int>(sheets.size());
    l.material = sheets.front().material;
    l.epsilon_complete = true;
    double h_over_er = 0, tand_h = 0;
    for (const Sheet& sh : sheets) {
      l.thickness += sh.h;
      l.epsilon_complete = l.epsilon_complete && sh.has_er;
      if (sh.has_er) h_over_er += static_cast<double>(sh.h) / sh.er;
      tand_h += sh.tand * static_cast<double>(sh.h);
    }
    if (sheets.size() == 1) {
      l.epsilon_r = sheets.front().er;
      l.loss_tangent = sheets.front().tand;
    } else if (l.epsilon_complete && h_over_er > 0) {
      l.epsilon_r = static_cast<double>(l.thickness) / h_over_er;  // series combination (stackup.hpp)
      l.loss_tangent = l.thickness > 0 ? tand_h / static_cast<double>(l.thickness) : 0;
    }
    return l;
  }

  NetId intern_net(const std::string& name) {
    if (auto it = b_.net_index.find(name); it != b_.net_index.end()) return it->second;
    const NetId id = static_cast<NetId>(b_.nets.size());
    b_.nets.push_back(model::Net{id, name, -1});
    b_.net_index[name] = id;
    return id;
  }

  // (net 3) / (net 3 "name") / (net "name")
  NetId read_net(NodeId parent) {
    const NodeId n = d_.find(parent, "net");
    if (n == kNoNode) return 0;
    const NodeId a = d_.child(n, 1);
    if (a == kNoNode) return 0;
    if (d_.node(a).kind == sexpr::Kind::String) return intern_net(d_.str(a));
    const int num = static_cast<int>(d_.number_at(n, 1).value_or(0));
    if (num == 0) return 0;
    if (auto it = by_number_.find(num); it != by_number_.end()) return it->second;
    // Number not in the table: use the name if given.
    const std::string name = d_.str_at(n, 2);
    if (!name.empty()) return intern_net(name);
    b_.warnings.push_back("unknown net number " + std::to_string(num) + " at line " + std::to_string(d_.line_of(d_.node(n).begin)));
    return 0;
  }

  LayerMask expand_copper(const std::string& name) const {
    if (name == "*.Cu") {
      LayerMask m = 0;
      for (int i = 0; i < b_.copper_count(); ++i) m |= model::layer_bit(i);
      return m;
    }
    if (name == "F&B.Cu") return model::layer_bit(0) | model::layer_bit(b_.copper_count() - 1);
    const int idx = b_.copper_index(name);
    return idx >= 0 ? model::layer_bit(idx) : 0;
  }
  LayerMask layers_mask(NodeId list, std::vector<std::string>* names) const {
    LayerMask m = 0;
    for (std::size_t i = 1; i < d_.children(list).size(); ++i) {
      const std::string s = d_.str_at(list, i);
      if (names) names->push_back(s);
      m |= expand_copper(s);
    }
    return m;
  }

  // ---------- sections ----------
  void read_layers(NodeId layers) {
    if (layers == kNoNode) throw std::runtime_error("missing (layers) section");
    for (NodeId l : d_.children(layers)) {
      if (!d_.is_list(l)) continue;
      model::LayerDef def;
      def.ordinal = static_cast<int>(d_.number_at(l, 0).value_or(-1));  // entries are headed by the ordinal
      def.file_name = d_.str_at(l, 1);
      def.name = def.file_name;
      def.type = d_.str_at(l, 2);
      // Copper layers are identified by number (old boards may name them "Front"/"Back"). Numbering changed
      // in KiCad 9: F.Cu 0, B.Cu 2, In1 4, In2 6, … (before: F.Cu 0, In1..In30 = 1..30, B.Cu 31).
      const bool v9 = b_.version >= 20240108;
      // Only copper ordinals can be copper: even ones from KiCad 9 on, 0..31 before. KiCad 10 boards may give
      // user layers a copper type ("(39 "User.1" signal)" in multichannel_mixer); they are not copper.
      const bool copper_ordinal = v9 ? def.ordinal >= 0 && def.ordinal % 2 == 0 : def.ordinal >= 0 && def.ordinal <= 31;
      const bool copper_type = copper_ordinal && (def.type == "signal" || def.type == "power" || def.type == "mixed" || def.type == "jumper");
      if (copper_type && !def.name.ends_with(".Cu")) {
        if (def.ordinal == 0) def.name = "F.Cu";
        else if (v9 ? def.ordinal == 2 : def.ordinal == 31) def.name = "B.Cu";
        else def.name = "In" + std::to_string(v9 ? (def.ordinal - 2) / 2 : def.ordinal) + ".Cu";
      }
      def.user_name = d_.str_at(l, 3);
      b_.layers.push_back(def);
    }
    // Copper stack order: F.Cu, In1.Cu … InN.Cu, B.Cu.
    std::vector<std::pair<int, int>> cu;  // (rank, layer index)
    for (std::size_t i = 0; i < b_.layers.size(); ++i) {
      const std::string& n = b_.layers[i].name;
      if (!n.ends_with(".Cu")) continue;
      int rank;
      if (n == "F.Cu") rank = 0;
      else if (n == "B.Cu") rank = 1'000'000;
      else if (n.starts_with("In")) rank = std::atoi(n.c_str() + 2);
      else continue;
      cu.emplace_back(rank, static_cast<int>(i));
    }
    std::sort(cu.begin(), cu.end());
    for (std::size_t k = 0; k < cu.size(); ++k) {
      b_.copper.push_back(cu[k].second);
      b_.layers[static_cast<std::size_t>(cu[k].second)].copper_index = static_cast<int>(k);
    }
    if (b_.copper.size() > 64) throw std::runtime_error("more than 64 copper layers");
  }

  void read_footprint(NodeId f) {
    model::Footprint fp;
    fp.node = f;
    fp.lib_id = d_.str_at(f, 1);
    fp.locked = yes(f, "locked");
    // The last copper layer, by its name in this file (KiCad 5 boards may call it "Back").
    if (NodeId l = d_.find(f, "layer"); l != kNoNode) {
      const std::string ln = d_.str_at(l, 1);
      fp.back = ln == "B.Cu" || (b_.copper_count() > 1 && b_.copper_index(ln) == b_.copper_count() - 1);
    }
    if (NodeId a = d_.find(f, "at"); a != kNoNode) {
      fp.pos = xy(a);
      fp.angle = d_.number_at(a, 3).value_or(0.0);
    }
    if (NodeId u = d_.find(f, "uuid"); u != kNoNode) fp.uuid = d_.str_at(u, 1);
    if (NodeId c = d_.find(f, "clearance"); c != kNoNode) fp.clearance = d_.nm_at(c, 1).value_or(-1);
    if (NodeId mm = d_.find(f, "solder_mask_margin"); mm != kNoNode) fp.mask_margin = d_.nm_at(mm, 1).value_or(INT64_MIN);
    if (NodeId nt = d_.find(f, "net_tie_pad_groups"); nt != kNoNode)
      for (std::size_t i = 1; i < d_.children(nt).size(); ++i) {
        std::vector<std::string> g;
        std::string cur;
        for (char ch : d_.str_at(nt, i) + ",") {
          if (ch == ',' || ch == ' ') {
            if (!cur.empty()) g.push_back(cur);
            cur.clear();
          } else {
            cur += ch;
          }
        }
        if (g.size() > 1) fp.net_tie_groups.push_back(std::move(g));
      }
    for (NodeId p : d_.find_all(f, "property")) {
      const std::string key = d_.str_at(p, 1);
      if (key == "Reference") fp.reference = d_.str_at(p, 2);
      else if (key == "Value") fp.value = d_.str_at(p, 2);
      else if (key == "Description" && fp.description.empty()) fp.description = d_.str_at(p, 2);
    }
    if (NodeId de = d_.find(f, "descr"); de != kNoNode && fp.description.empty()) fp.description = d_.str_at(de, 1);
    if (NodeId tg = d_.find(f, "tags"); tg != kNoNode) fp.keywords = d_.str_at(tg, 1);
    for (NodeId t : d_.find_all(f, "fp_text")) {  // KiCad <= 7
      const std::string kind = d_.str_at(t, 1);
      if (kind == "reference" && fp.reference.empty()) fp.reference = d_.str_at(t, 2);
      if (kind == "value" && fp.value.empty()) fp.value = d_.str_at(t, 2);
    }
    if (NodeId at = d_.find(f, "attr"); at != kNoNode) {
      fp.attr_smd = has_symbol(at, "smd");
      fp.attr_through_hole = has_symbol(at, "through_hole");
      fp.board_only = has_symbol(at, "board_only");
      fp.exclude_from_pos = has_symbol(at, "exclude_from_pos_files");
      fp.exclude_from_bom = has_symbol(at, "exclude_from_bom");
      fp.allow_missing_courtyard = has_symbol(at, "allow_missing_courtyard");
      fp.dnp = has_symbol(at, "dnp");
    }
    if (NodeId dn = d_.find(f, "dnp"); dn != kNoNode) fp.dnp = yes(f, "dnp");
    const int fi = static_cast<int>(b_.footprints.size());
    b_.footprints.push_back(fp);
    for (NodeId c : d_.children(f)) {
      if (!d_.is_list(c)) continue;
      const std::string_view h = d_.head(c);
      if (h == "pad") read_pad(c, fi);
      else if (h.starts_with("fp_") && h != "fp_text" && h != "fp_text_box") read_graphic(c, fi, fp.pos, fp.angle, true);
      else if (h == "fp_text") read_text(c, fi, fp.pos, fp.angle);
      else if (h == "zone") read_zone(c, fi);
    }
  }

  void read_pad(NodeId p, int fi) {
    model::Footprint& fp = b_.footprints[static_cast<std::size_t>(fi)];
    model::Pad pad;
    pad.node = p;
    pad.footprint = fi;
    pad.number = d_.str_at(p, 1);
    const std::string type = d_.str_at(p, 2), shape = d_.str_at(p, 3);
    if (type == "thru_hole") pad.type = model::PadType::ThruHole;
    else if (type == "np_thru_hole") pad.type = model::PadType::NpThruHole;
    else if (type == "connect") pad.type = model::PadType::Connect;
    else pad.type = model::PadType::Smd;
    if (shape == "circle") pad.shape = model::PadShape::Circle;
    else if (shape == "oval") pad.shape = model::PadShape::Oval;
    else if (shape == "trapezoid") pad.shape = model::PadShape::Trapezoid;
    else if (shape == "roundrect") pad.shape = model::PadShape::RoundRect;
    else if (shape == "chamfered_rect") pad.shape = model::PadShape::ChamferedRect;
    else if (shape == "custom") pad.shape = model::PadShape::Custom;
    else pad.shape = model::PadShape::Rect;
    Point local{};
    if (NodeId a = d_.find(p, "at"); a != kNoNode) {
      local = xy(a);
      pad.angle = d_.number_at(a, 3).value_or(0.0);
    }
    pad.pos = fp.pos + geom::rotate(local, fp.angle);
    if (NodeId s = d_.find(p, "size"); s != kNoNode) {
      pad.size_x = d_.nm_at(s, 1).value_or(0);
      pad.size_y = d_.nm_at(s, 2).value_or(pad.size_x);
    }
    if (NodeId dr = d_.find(p, "drill"); dr != kNoNode) {
      std::size_t i = 1;
      if (d_.str_at(dr, 1) == "oval") {
        pad.drill_oval = true;
        i = 2;
      }
      pad.drill_x = d_.nm_at(dr, i).value_or(0);
      pad.drill_y = d_.nm_at(dr, i + 1).value_or(pad.drill_x);
      if (NodeId off = d_.find(dr, "offset"); off != kNoNode) pad.drill_offset = xy(off);
    }
    if (NodeId l = d_.find(p, "layers"); l != kNoNode) pad.copper = layers_mask(l, &pad.layers);
    // A plated through hole has copper on every copper layer, whatever the file lists (KiCad semantics;
    // verified on PCBench WordClock where P3 lists only F.Cu and KiCad's DRC checks it on B.Cu).
    if (pad.type == model::PadType::ThruHole) pad.copper = expand_copper("*.Cu");
    // A pad without a hole has copper on one side only: KiCad connects an SMD pad listed on "*.Cu" on its
    // footprint's side and reports a track reaching it on the other side as a missing connection (PCBench
    // Kefersender SMA_E1: pads 0 and 2, routed on B.Cu, stayed in KiCad's ratsnest).
    if ((pad.type == model::PadType::Smd || pad.type == model::PadType::Connect) && pad.drill_x == 0 && fi >= 0) {
      const model::LayerMask front = model::layer_bit(0), back = model::layer_bit(b_.copper_count() - 1);
      if ((pad.copper & front) && (pad.copper & back) && b_.copper_count() > 1) pad.copper = b_.footprints[static_cast<std::size_t>(fi)].back ? back : front;
    }
    if (NodeId r = d_.find(p, "roundrect_rratio"); r != kNoNode) pad.roundrect_ratio = d_.number_at(r, 1).value_or(0);
    if (NodeId r = d_.find(p, "chamfer_ratio"); r != kNoNode) pad.chamfer_ratio = d_.number_at(r, 1).value_or(0);
    if (NodeId c = d_.find(p, "chamfer"); c != kNoNode) {
      if (has_symbol(c, "top_left")) pad.chamfer_corners |= 1;
      if (has_symbol(c, "top_right")) pad.chamfer_corners |= 2;
      if (has_symbol(c, "bottom_left")) pad.chamfer_corners |= 4;
      if (has_symbol(c, "bottom_right")) pad.chamfer_corners |= 8;
    }
    if (NodeId r = d_.find(p, "rect_delta"); r != kNoNode) {
      pad.trapezoid_dx = d_.nm_at(r, 1).value_or(0);
      pad.trapezoid_dy = d_.nm_at(r, 2).value_or(0);
    }
    if (NodeId c = d_.find(p, "clearance"); c != kNoNode) pad.clearance = d_.nm_at(c, 1).value_or(-1);
    if (NodeId mm = d_.find(p, "solder_mask_margin"); mm != kNoNode) pad.mask_margin = d_.nm_at(mm, 1).value_or(INT64_MIN);
    if (NodeId prim = d_.find(p, "primitives"); prim != kNoNode) {
      for (NodeId g : d_.find_all(prim, "gr_poly")) {
        std::vector<Point> poly;
        if (NodeId pts = d_.find(g, "pts"); pts != kNoNode) read_pts(pts, poly, Point{}, 0);
        pad.custom_polys.push_back(std::move(poly));
      }
    }
    if (NodeId pf = d_.find(p, "pinfunction"); pf != kNoNode) pad.pinfunction = d_.str_at(pf, 1);
    if (NodeId pt = d_.find(p, "pintype"); pt != kNoNode) pad.pintype = d_.str_at(pt, 1);
    pad.net = read_net(p);
    fp.pads.push_back(static_cast<int>(b_.pads.size()));
    b_.pads.push_back(std::move(pad));
  }

  // Appends the points of a (pts (xy ..) (arc (start)(mid)(end)) ...) list, transformed by origin + rotation.
  void read_pts(NodeId pts, std::vector<Point>& out, Point origin, double angle) const {
    for (NodeId c : d_.children(pts)) {
      if (!d_.is_list(c)) continue;
      const std::string_view h = d_.head(c);
      if (h == "xy") {
        out.push_back(origin + geom::rotate(xy(c), angle));
      } else if (h == "arc") {
        // Outline arcs (zone fills around round pads, rounded board corners): flatten with 0.1 µm chords so
        // clearances measured to the outline stay exact within KiCad's DRC epsilon.
        const NodeId s = d_.find(c, "start"), m = d_.find(c, "mid"), e = d_.find(c, "end");
        if (s == kNoNode || m == kNoNode || e == kNoNode) continue;
        const auto arc = geom::arc_points(origin + geom::rotate(xy(s), angle), origin + geom::rotate(xy(m), angle),
                                          origin + geom::rotate(xy(e), angle), 100);
        out.insert(out.end(), arc.begin(), arc.end());
      }
    }
  }

  void read_graphic(NodeId g, int fi, Point origin, double angle, bool local) {
    model::Graphic gr;
    gr.node = g;
    gr.footprint = fi;
    if (NodeId l = d_.find(g, "layer"); l != kNoNode) gr.layer = d_.str_at(l, 1);
    // Keep only layers that matter for routing and placement.
    const bool keep = gr.layer == "Edge.Cuts" || b_.copper_index(gr.layer) >= 0 || gr.layer.ends_with(".CrtYd") ||
                      gr.layer == "Margin" || gr.layer == "F.Mask" || gr.layer == "B.Mask";
    if (!keep) return;
    const std::string_view h = d_.head(g);
    const std::string_view kind = h.substr(3);  // after "gr_" / "fp_"
    auto tf = [&](Point p) { return local ? origin + geom::rotate(p, angle) : p; };
    if (NodeId s = d_.find(g, "stroke"); s != kNoNode) gr.width = child_nm(s, "width");
    else gr.width = child_nm(g, "width");
    if (NodeId fl = d_.find(g, "fill"); fl != kNoNode) {
      const std::string v = d_.str_at(fl, 1);
      gr.filled = v == "solid" || v == "yes";
    } else {
      // KiCad <= 6 wrote no fill token: polygons were always filled, other shapes never.
      const std::string_view k2 = d_.head(g).substr(3);
      gr.filled = k2 == "poly";
    }
    if (kind == "line") {
      gr.kind = model::Graphic::Kind::Line;
      gr.a = tf(child_xy(g, "start"));
      gr.b = tf(child_xy(g, "end"));
    } else if (kind == "arc") {
      gr.kind = model::Graphic::Kind::Arc;
      if (d_.find(g, "mid") != kNoNode) {
        gr.a = tf(child_xy(g, "start"));
        gr.c = tf(child_xy(g, "mid"));
        gr.b = tf(child_xy(g, "end"));
      } else {
        // KiCad <= 5: (start CENTRE) (end START-POINT) (angle SWEEP), sweep clockwise on screen.
        const Point centre = child_xy(g, "start"), p0 = child_xy(g, "end");
        double sweep = 0;
        if (NodeId an = d_.find(g, "angle"); an != kNoNode) sweep = d_.number_at(an, 1).value_or(0.0);
        const Point rel = p0 - centre;
        if (sweep == 0.0) {
          // KiCad reads a legacy arc with angle 0 as a full circle (`kicad-cli pcb upgrade` writes start = end and
          // the opposite point as mid). On Edge.Cuts that is a round cut-out (PCBench kitspace_d20_tri_r1.0).
          gr.kind = model::Graphic::Kind::Circle;
          gr.a = tf(centre);
          gr.b = tf(p0);
        } else {
          gr.a = tf(p0);
          gr.c = tf(centre + geom::rotate(rel, -sweep / 2));
          gr.b = tf(centre + geom::rotate(rel, -sweep));
        }
      }
    } else if (kind == "circle") {
      gr.kind = model::Graphic::Kind::Circle;
      gr.a = tf(child_xy(g, "center"));
      gr.b = tf(child_xy(g, "end"));
    } else if (kind == "rect") {
      gr.kind = model::Graphic::Kind::Rect;
      // A rotated footprint turns the rectangle into a polygon; store all four corners.
      const Point s = child_xy(g, "start"), e = child_xy(g, "end");
      gr.a = tf(s);
      gr.b = tf(e);
      gr.pts = {tf(s), tf(Point{e.x, s.y}), tf(e), tf(Point{s.x, e.y})};
    } else if (kind == "poly") {
      gr.kind = model::Graphic::Kind::Poly;
      if (NodeId pts = d_.find(g, "pts"); pts != kNoNode) read_pts(pts, gr.pts, local ? origin : Point{}, local ? angle : 0);
    } else if (kind == "curve") {
      gr.kind = model::Graphic::Kind::Curve;
      if (NodeId pts = d_.find(g, "pts"); pts != kNoNode) read_pts(pts, gr.pts, local ? origin : Point{}, local ? angle : 0);
    } else {
      return;
    }
    gr.net = read_net(g);
    if (fi >= 0) b_.footprints[static_cast<std::size_t>(fi)].graphics.push_back(static_cast<int>(b_.graphics.size()));
    b_.graphics.push_back(std::move(gr));
  }

  void read_text(NodeId t, int fi, Point origin, double angle) {
    model::Text tx;
    tx.footprint = fi;
    if (NodeId l = d_.find(t, "layer"); l != kNoNode) tx.layer = d_.str_at(l, 1);
    // Copper text is an obstacle; text on a solder-mask layer opens the mask (PCBench MySensorIRBlaster, FRM16).
    if (b_.copper_index(tx.layer) < 0 && tx.layer != "F.Mask" && tx.layer != "B.Mask") return;
    const bool fp_text = d_.head(t) == "fp_text";
    tx.text = d_.str_at(t, fp_text ? 2 : 1);
    Point p{};
    if (NodeId a = d_.find(t, "at"); a != kNoNode) {
      p = xy(a);
      tx.angle = d_.number_at(a, 3).value_or(0.0);  // absolute in KiCad files
    }
    tx.pos = fi >= 0 ? origin + geom::rotate(p, angle) : p;
    tx.hidden = yes(t, "hide");
    if (NodeId e = d_.find(t, "effects"); e != kNoNode) {
      if (NodeId f = d_.find(e, "font"); f != kNoNode) {
        if (NodeId s = d_.find(f, "size"); s != kNoNode) {
          tx.height = d_.nm_at(s, 1).value_or(0);
          tx.width = d_.nm_at(s, 2).value_or(tx.height);
        }
        tx.thickness = child_nm(f, "thickness");
      }
      if (NodeId j = d_.find(e, "justify"); j != kNoNode) {
        if (has_symbol(j, "left")) tx.justify_h = -1;
        if (has_symbol(j, "right")) tx.justify_h = 1;
        if (has_symbol(j, "top")) tx.justify_v = -1;
        if (has_symbol(j, "bottom")) tx.justify_v = 1;
        tx.mirror = has_symbol(j, "mirror");
      }
      if (yes(e, "hide")) tx.hidden = true;
    }
    b_.texts.push_back(std::move(tx));
  }

  void read_segment(NodeId s) {
    model::Track t;
    t.node = s;
    t.a = child_xy(s, "start");
    t.b = child_xy(s, "end");
    t.width = child_nm(s, "width");
    if (NodeId l = d_.find(s, "layer"); l != kNoNode) t.layer = b_.copper_index(d_.str_at(l, 1));
    t.net = read_net(s);
    t.locked = yes(s, "locked");
    b_.tracks.push_back(t);
  }

  void read_arc(NodeId s) {
    model::ArcTrack t;
    t.node = s;
    t.a = child_xy(s, "start");
    t.mid = child_xy(s, "mid");
    t.b = child_xy(s, "end");
    t.width = child_nm(s, "width");
    if (NodeId l = d_.find(s, "layer"); l != kNoNode) t.layer = b_.copper_index(d_.str_at(l, 1));
    t.net = read_net(s);
    t.locked = yes(s, "locked");
    b_.arcs.push_back(t);
  }

  void read_via(NodeId v) {
    model::Via via;
    via.node = v;
    if (has_symbol(v, "blind")) via.type = model::ViaType::Blind;
    else if (has_symbol(v, "micro")) via.type = model::ViaType::Micro;
    if (NodeId t = d_.find(v, "type"); t != kNoNode) {  // possible newer syntax
      const std::string s = d_.str_at(t, 1);
      if (s == "blind" || s == "buried") via.type = model::ViaType::Blind;
      if (s == "micro") via.type = model::ViaType::Micro;
    }
    via.pos = child_xy(v, "at");
    via.size = child_nm(v, "size");
    via.drill = child_nm(v, "drill");
    via.layer_top = 0;
    via.layer_bottom = b_.copper_count() - 1;
    if (NodeId l = d_.find(v, "layers"); l != kNoNode) {
      const int a = b_.copper_index(d_.str_at(l, 1)), c = b_.copper_index(d_.str_at(l, 2));
      if (a >= 0 && c >= 0) {
        via.layer_top = std::min(a, c);
        via.layer_bottom = std::max(a, c);
      }
    }
    via.net = read_net(v);
    via.locked = yes(v, "locked");
    via.free = yes(v, "free");
    b_.vias.push_back(via);
  }

  void read_zone(NodeId z, int fi) {
    model::Zone zone;
    zone.node = z;
    zone.footprint = fi;
    zone.net = read_net(z);
    if (zone.net == 0)
      if (NodeId nn = d_.find(z, "net_name"); nn != kNoNode) {
        const std::string s = d_.str_at(nn, 1);
        if (!s.empty()) zone.net = intern_net(s);
      }
    if (NodeId l = d_.find(z, "layer"); l != kNoNode) zone.copper |= layers_mask(l, &zone.layers);
    if (NodeId l = d_.find(z, "layers"); l != kNoNode) zone.copper |= layers_mask(l, &zone.layers);
    if (NodeId n = d_.find(z, "name"); n != kNoNode) zone.name = d_.str_at(n, 1);
    if (NodeId p = d_.find(z, "priority"); p != kNoNode) zone.priority = static_cast<int>(d_.number_at(p, 1).value_or(0));
    if (NodeId cp = d_.find(z, "connect_pads"); cp != kNoNode)
      if (NodeId c = d_.find(cp, "clearance"); c != kNoNode) zone.clearance = d_.nm_at(c, 1).value_or(-1);
    if (NodeId k = d_.find(z, "keepout"); k != kNoNode) {
      zone.rule_area = true;
      auto na = [&](std::string_view what) {
        const NodeId c = d_.find(k, what);
        return c != kNoNode && d_.str_at(c, 1) == "not_allowed";
      };
      zone.keepout_tracks = na("tracks");
      zone.keepout_vias = na("vias");
      zone.keepout_pads = na("pads");
      zone.keepout_pour = na("copperpour");
      zone.keepout_footprints = na("footprints");
    }
    for (NodeId poly : d_.find_all(z, "polygon")) {
      std::vector<Point> pts;
      if (NodeId p = d_.find(poly, "pts"); p != kNoNode) read_pts(p, pts, Point{}, 0);
      zone.outline.push_back(std::move(pts));
    }
    for (NodeId fill : d_.find_all(z, "filled_polygon")) {
      int li = -1;
      if (NodeId l = d_.find(fill, "layer"); l != kNoNode) li = b_.copper_index(d_.str_at(l, 1));
      std::vector<Point> pts;
      if (NodeId p = d_.find(fill, "pts"); p != kNoNode) read_pts(p, pts, Point{}, 0);
      zone.fills.emplace_back(li, std::move(pts));
    }
    b_.zones.push_back(std::move(zone));
  }

  const Document& d_;
  Board b_;
  std::unordered_map<int, NetId> by_number_;
};

}  // namespace

model::Board read_board(const sexpr::Document& doc) {
  // Edits are text replacements applied by write(); the parsed tree still holds the original text. Reading an
  // edited document must give the edited board, so parse the written text (found by the placer: reading an
  // edited tree returned the board before the edits).
  if (doc.modified()) {
    const sexpr::Document fresh = sexpr::Document::parse(doc.write());
    return Reader(fresh).run();
  }
  return Reader(doc).run();
}

LoadedBoard read_board_file(const std::string& path) {
  LoadedBoard lb{sexpr::Document::load(path), {}};
  lb.board = read_board(lb.doc);
  return lb;
}

}  // namespace tmk::io
