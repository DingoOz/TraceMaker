// SPDX-License-Identifier: GPL-3.0-or-later
// SRJ problem -> KiCad 10 board and project (design doc 08 §9).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <numbers>

#include "core/rng.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/srj/srj.hpp"
#include "sexpr/sexpr.hpp"

namespace tmk::io {
namespace {
using sexpr::format_mm;
using sexpr::quote;

// tscircuit's isConnectionPointOnObstacle accepts a point this far outside the rectangle.
constexpr double kOnObstacleTolMm = 1e-3;

std::string cu_name(int z, int layer_count) {
  if (z <= 0) return "F.Cu";
  if (z >= layer_count - 1) return "B.Cu";
  return "In" + std::to_string(z) + ".Cu";
}

struct PadOut {
  std::string type = "smd", shape = "rect";
  Coord x = 0, y = 0, w = 0, h = 0, drill_x = 0, drill_y = 0;
  double angle = 0;
  std::vector<int> layers;  // empty = every copper layer (through hole)
  int net = -1;
};

struct FootprintOut {
  std::string name;
  std::vector<PadOut> pads;
};

struct ZoneOut {
  std::string name;
  int net = -1;
  bool keepout = false;
  std::vector<int> layers;
  std::vector<std::pair<Coord, Coord>> pts;
  bool vias_ok = false;  // keep-out for tracks only
};

class Writer {
 public:
  Writer(const SrjProblem& p, const SrjImportOptions& opt) : p_(p), opt_(opt) {}

  SrjBoardFiles run() {
    out_.warnings = p_.warnings;
    resolve_rules();
    collect();
    out_.pcb = board_text();
    out_.pro = project_text();
    out_.dru = rules_text();
    return std::move(out_);
  }

 private:
  // y is negated: SRJ is y up, KiCad y down. The same picture on screen, so ccwRotationDegrees is KiCad's angle.
  static Coord kx(double x) { return mm_to_nm(x); }
  static Coord ky(double y) { return -mm_to_nm(y); }

  void warn(const std::string& w) { out_.warnings.push_back(w); }

  void resolve_rules() {
    const auto& r = p_.rules;
    if (opt_.clearance) {
      out_.clearance = *opt_.clearance;
    } else if (r.obstacle_margin) {
      out_.clearance = mm_to_nm(*r.obstacle_margin);
    } else {
      out_.clearance = mm_to_nm(kSrjDefaultClearanceMm);
      warn("the problem has no defaultObstacleMargin: copper clearance set to tscircuit's fallback of " + format_mm(out_.clearance) + " mm");
    }
    out_.hole_to_hole = opt_.hole_to_hole ? *opt_.hole_to_hole : mm_to_nm(std::max(r.via_hole_to_via_hole.value_or(0.0), r.drill_to_drill.value_or(0.0)));
    out_.track_width = mm_to_nm(r.nominal_trace_width.value_or(r.min_trace_width));
    if (out_.track_width < mm_to_nm(r.min_trace_width)) {
      warn("nominalTraceWidth is below minTraceWidth: routing at minTraceWidth");
      out_.track_width = mm_to_nm(r.min_trace_width);
    }
    // getViaDimensions (tscircuit-autorouter lib/utils/getViaDimensions.ts).
    const double pad = std::max(r.via_pad.value_or(kSrjDefaultViaPadMm), r.via_hole.value_or(0.0));
    out_.via_diameter = mm_to_nm(pad);
    out_.via_drill = mm_to_nm(r.via_hole.value_or(pad * 0.5));
    if (!r.via_pad || !r.via_hole)
      warn("the problem does not give both via diameters: using tscircuit's fallback, pad " + format_mm(out_.via_diameter) + " mm, hole " +
           format_mm(out_.via_drill) + " mm");
    if (out_.via_drill >= out_.via_diameter) warn("the via hole is not smaller than the via pad: vias have no annular ring");
    if (r.pad_to_pad) warn("minPadEdgeToPadEdgeClearance concerns the fixed pads only and is not written as a rule");
    if (r.via_in_pad) warn("the problem allows via-in-pad: pass --via-in-pad to `route` to use it");
  }

  Coord net_width(int net) const {
    Coord w = 0;
    for (int c : p_.nets[static_cast<std::size_t>(net)].connections)
      if (const auto& k = p_.connections[static_cast<std::size_t>(c)]; k.width) w = std::max(w, mm_to_nm(*k.width));
    return w > 0 ? std::max(w, mm_to_nm(p_.rules.min_trace_width)) : out_.track_width;
  }

 public:
  static bool contains(const SrjObstacle& o, double x, double y) {
    const double dx = x - o.x, dy = y - o.y;
    if (o.circle) return std::hypot(dx, dy) <= o.width / 2 + kOnObstacleTolMm;
    const double a = -o.rotation * std::numbers::pi / 180.0;  // into the rectangle's own frame
    const double lx = dx * std::cos(a) - dy * std::sin(a), ly = dx * std::sin(a) + dy * std::cos(a);
    return std::abs(lx) <= o.width / 2 + kOnObstacleTolMm && std::abs(ly) <= o.height / 2 + kOnObstacleTolMm;
  }

 private:
  static std::vector<std::pair<Coord, Coord>> outline_of(const SrjObstacle& o) {
    std::vector<std::pair<Coord, Coord>> pts;
    if (o.circle) {
      for (int i = 0; i < 64; ++i) {
        const double a = 2 * std::numbers::pi * i / 64;
        pts.emplace_back(kx(o.x + o.width / 2 * std::cos(a)), ky(o.y + o.width / 2 * std::sin(a)));
      }
      return pts;
    }
    const double a = o.rotation * std::numbers::pi / 180.0, c = std::cos(a), s = std::sin(a);
    for (const auto& [sx, sy] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) {
      const double lx = sx * o.width / 2, ly = sy * o.height / 2;
      pts.emplace_back(kx(o.x + lx * c - ly * s), ky(o.y + lx * s + ly * c));
    }
    return pts;
  }

  FootprintOut& footprint(const std::string& name) {
    const auto [it, fresh] = fp_index_.emplace(name, fps_.size());
    if (fresh) fps_.push_back(FootprintOut{name, {}});
    return fps_[it->second];
  }

  void collect() {
    const int n = p_.layer_count;
    int pours = 0, plated = 0, partial = 0, rect_holes = 0;
    std::vector<int> pads_of_net(p_.nets.size(), 0);
    for (std::size_t i = 0; i < p_.obstacles.size(); ++i) {
      const auto& o = p_.obstacles[i];
      const std::string label = o.id.empty() ? "obstacle" + std::to_string(i) : o.id;
      if (o.copper_pour) {
        // A pour is copper that is poured again around the routing, which is what an unfilled KiCad zone is.
        ++pours;
        zones_.push_back(ZoneOut{label, o.net, false, o.layers, outline_of(o)});
        continue;
      }
      PadOut pad;
      pad.x = kx(o.x);
      pad.y = ky(o.y);
      pad.w = mm_to_nm(o.width);
      pad.h = mm_to_nm(o.height);
      pad.angle = o.circle ? 0.0 : o.rotation;
      pad.shape = o.circle ? "circle" : "rect";
      FootprintOut& fp = footprint(o.component.empty() ? "SRJ" : o.component);
      if (o.non_plated_hole) {
        pad.type = "np_thru_hole";
        pad.drill_x = pad.w;
        pad.drill_y = pad.h;
        if (!o.circle) {
          // KiCad drills are round or oval. The slot leaves the rectangle's corners out, so a keep-out covers them.
          ++rect_holes;
          pad.shape = "oval";
          zones_.push_back(ZoneOut{label, -1, true, {}, outline_of(o)});
        }
        fp.pads.push_back(pad);
        continue;
      }
      pad.net = o.net;
      if (o.net >= 0 && o.layers.size() > 1) {
        // A connected obstacle on several layers is a plated hole. SRJ has no drill size; tscircuit's own
        // conversion (lib/testing/utils/convertToCircuitJson.ts) drills half the smaller side, at least 0.1 mm.
        ++plated;
        partial += static_cast<int>(o.layers.size()) < n;
        pad.type = "thru_hole";
        pad.drill_x = pad.drill_y = std::max<Coord>(std::min(pad.w, pad.h) / 2, 100'000);
        fp.pads.push_back(pad);
        ++pads_of_net[static_cast<std::size_t>(o.net)];
        continue;
      }
      // One pad per layer: without a net nothing has to join the layers, so no hole is invented.
      for (int z : o.layers) {
        pad.layers = {z};
        fp.pads.push_back(pad);
        if (o.net >= 0) ++pads_of_net[static_cast<std::size_t>(o.net)];
      }
    }
    // Every point needs a pad of its net under it; a point on bare board gets one the size of the track end.
    std::map<std::tuple<int, Coord, Coord, int>, int> made;
    int foreign = 0, first_layer_only = 0;
    for (const auto& c : p_.connections) {
      out_.srj_pairs += std::max<int>(0, static_cast<int>(c.points.size()) - 1);
      for (const auto& pt : c.points) {
        bool on_own = false, on_other = false;
        for (const auto& o : p_.obstacles) {
          if (o.copper_pour || o.non_plated_hole || !contains(o, pt.x, pt.y)) continue;
          const bool shares = std::any_of(pt.layers.begin(), pt.layers.end(), [&](int z) { return std::find(o.layers.begin(), o.layers.end(), z) != o.layers.end(); });
          if (!shares) continue;
          (o.net == c.net ? on_own : on_other) = true;
        }
        foreign += on_other;
        if (on_own) continue;
        const int z = pt.layers.front();
        first_layer_only += pt.layers.size() > 1;
        if (!made.emplace(std::tuple{c.net, kx(pt.x), ky(pt.y), z}, 1).second) continue;
        PadOut pad;
        pad.shape = "circle";
        pad.x = kx(pt.x);
        pad.y = ky(pt.y);
        pad.w = pad.h = net_width(c.net);
        pad.layers = {z};
        pad.net = c.net;
        footprint("SRJ_TERMINALS").pads.push_back(pad);
        ++out_.terminal_pads;
        ++pads_of_net[static_cast<std::size_t>(c.net)];
      }
    }
    for (int k : pads_of_net) out_.board_pairs += std::max(0, k - 1);
    for (const auto& fp : fps_) out_.pads += static_cast<int>(fp.pads.size());
    out_.nets = static_cast<int>(p_.nets.size());
    if (n == 1) {
      // KiCad boards have two copper layers at least: the second one is closed to tracks and vias.
      std::vector<std::pair<Coord, Coord>> all{{kx(p_.min_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.min_y)}, {kx(p_.min_x), ky(p_.min_y)}};
      zones_.push_back(ZoneOut{"srj_single_layer", -1, true, {1}, std::move(all)});
      warn("layerCount is 1: the board has a B.Cu layer closed to tracks and vias by a rule area");
    }
    if (!p_.track_layers.empty()) {
      std::vector<std::pair<Coord, Coord>> all{{kx(p_.min_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.min_y)}, {kx(p_.min_x), ky(p_.min_y)}};
      std::vector<int> closed;
      for (int z = 0; z < n; ++z)
        if (std::find(p_.track_layers.begin(), p_.track_layers.end(), z) == p_.track_layers.end()) closed.push_back(z);
      if (!closed.empty()) {
        ZoneOut zo{"srj_no_tracks", -1, true, closed, std::move(all)};
        zo.vias_ok = true;
        zones_.push_back(std::move(zo));
      }
    }
    for (const auto& pl : p_.planes) {  // plane terminations (buses): a zone of the net over the whole board on that layer
      std::vector<std::pair<Coord, Coord>> all{{kx(p_.min_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.max_y)}, {kx(p_.max_x), ky(p_.min_y)}, {kx(p_.min_x), ky(p_.min_y)}};
      zones_.push_back(ZoneOut{"srj_plane_" + std::to_string(pl.layer), pl.net, false, {pl.layer}, std::move(all)});
    }
    if (pours) warn(std::to_string(pours) + " copper pours written as unfilled zones: refill them in KiCad after routing");
    if (plated)
      warn(std::to_string(plated) + " connected multi-layer obstacles written as plated through-hole pads drilled at half their smaller side (SRJ has no drill size)");
    if (partial) warn(std::to_string(partial) + " of them do not span the whole stack in the problem; a KiCad through-hole pad does");
    if (rect_holes) warn(std::to_string(rect_holes) + " rectangular non-plated holes written as slots with a keep-out over the rectangle");
    if (out_.terminal_pads) warn(std::to_string(out_.terminal_pads) + " connection points lie on no obstacle of their net: round pads of the track width were added for them");
    if (first_layer_only) warn(std::to_string(first_layer_only) + " multi-layer points without an obstacle can be reached on their first layer only");
    if (foreign) warn(std::to_string(foreign) + " connection points lie on an obstacle that is not connected to them");
    if (out_.srj_pairs != out_.board_pairs)
      warn("the problem asks for " + std::to_string(out_.srj_pairs) + " point-to-point connections but its nets need " + std::to_string(out_.board_pairs) +
           " (a pad without a point, or connections that do not form a tree)");
  }

  std::string uuid() {
    // Counted, not drawn: the n-th UUID of every import is the same (Philox block n of a fixed stream).
    const U32x4 b = RngStream(0x53524Au, 0x1A90u, 0).block(uuid_counter_++);
    char s[40];
    std::snprintf(s, sizeof s, "%08x-%04x-4%03x-%04x-%04x%08x", b.v[0], b.v[1] >> 16, b.v[1] & 0x0FFFu, (b.v[2] >> 16 & 0x3FFFu) | 0x8000u,
                  b.v[2] & 0xFFFFu, b.v[3]);
    return s;
  }

  std::string layer_names(const std::vector<int>& layers, bool with_mask) const {
    const int n = std::max(2, p_.layer_count);
    if (layers.empty()) return with_mask ? "\"*.Cu\" \"*.Mask\"" : "\"*.Cu\"";
    std::string s;
    for (int z : layers) {
      s += (s.empty() ? "" : " ") + quote(cu_name(z, n));
      if (with_mask && z == 0) s += " \"F.Mask\"";
      if (with_mask && z == n - 1) s += " \"B.Mask\"";
    }
    return s;
  }

  std::string property(const char* key, const std::string& value, const char* layer) {
    return std::string("\t\t(property ") + quote(key) + " " + quote(value) + "\n\t\t\t(at 0 0 0)\n\t\t\t(layer " + quote(layer) +
           ")\n\t\t\t(hide yes)\n\t\t\t(uuid " + quote(uuid()) + ")\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n";
  }

  std::string board_text() {
    const int n = std::max(2, p_.layer_count);
    std::string s = "(kicad_pcb\n\t(version 20260206)\n\t(generator \"tracemaker\")\n\t(generator_version \"10.0\")\n"
                    "\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)\n\t(paper \"A4\")\n\t(layers\n\t\t(0 \"F.Cu\" signal)\n";
    for (int z = 1; z < n - 1; ++z) s += "\t\t(" + std::to_string(2 * z + 2) + " \"In" + std::to_string(z) + ".Cu\" signal)\n";
    s += "\t\t(2 \"B.Cu\" signal)\n\t\t(9 \"F.Adhes\" user \"F.Adhesive\")\n\t\t(11 \"B.Adhes\" user \"B.Adhesive\")\n\t\t(13 \"F.Paste\" user)\n"
         "\t\t(15 \"B.Paste\" user)\n\t\t(5 \"F.SilkS\" user \"F.Silkscreen\")\n\t\t(7 \"B.SilkS\" user \"B.Silkscreen\")\n\t\t(1 \"F.Mask\" user)\n"
         "\t\t(3 \"B.Mask\" user)\n\t\t(17 \"Dwgs.User\" user \"User.Drawings\")\n\t\t(19 \"Cmts.User\" user \"User.Comments\")\n"
         "\t\t(21 \"Eco1.User\" user \"User.Eco1\")\n\t\t(23 \"Eco2.User\" user \"User.Eco2\")\n\t\t(25 \"Edge.Cuts\" user)\n\t\t(27 \"Margin\" user)\n"
         "\t\t(31 \"F.CrtYd\" user \"F.Courtyard\")\n\t\t(29 \"B.CrtYd\" user \"B.Courtyard\")\n\t\t(35 \"F.Fab\" user)\n\t\t(33 \"B.Fab\" user)\n\t)\n"
         "\t(setup\n\t\t(pad_to_mask_clearance 0)\n\t\t(allow_soldermask_bridges_in_footprints no)\n\t\t(tenting\n\t\t\t(front yes)\n\t\t\t(back yes)\n\t\t)\n\t)\n";
    for (const auto& fp : fps_) {
      // On the back when every pad is: KiCad puts a hole-less pad on its footprint's side.
      const bool back = std::all_of(fp.pads.begin(), fp.pads.end(), [&](const PadOut& q) { return q.layers.size() == 1 && q.layers[0] == n - 1; });
      const bool through = std::any_of(fp.pads.begin(), fp.pads.end(), [](const PadOut& q) { return q.type == "thru_hole"; });
      s += "\t(footprint " + quote("SRJ:" + fp.name) + "\n\t\t(layer " + (back ? "\"B.Cu\"" : "\"F.Cu\"") + ")\n\t\t(uuid " + quote(uuid()) + ")\n\t\t(at 0 0)\n";
      s += property("Reference", fp.name, back ? "B.Fab" : "F.Fab");
      s += property("Value", "SRJ", back ? "B.Fab" : "F.Fab");
      s += std::string("\t\t(attr ") + (through ? "through_hole" : "smd") + " board_only exclude_from_pos_files exclude_from_bom)\n";
      int number = 0;
      for (const auto& q : fp.pads) {
        const std::string a = format_angle(q.angle);
        s += "\t\t(pad " + quote(q.type == "np_thru_hole" ? std::string() : std::to_string(++number)) + " " + q.type + " " + q.shape + "\n";
        s += "\t\t\t(at " + format_mm(q.x) + " " + format_mm(q.y) + (a == "0" ? "" : " " + a) + ")\n";
        s += "\t\t\t(size " + format_mm(q.w) + " " + format_mm(q.h) + ")\n";
        if (q.drill_x > 0) s += "\t\t\t(drill " + (q.drill_x != q.drill_y ? "oval " + format_mm(q.drill_x) + " " + format_mm(q.drill_y) : format_mm(q.drill_x)) + ")\n";
        s += "\t\t\t(layers " + layer_names(q.layers, true) + ")\n";
        if (q.net >= 0) s += "\t\t\t(net " + quote(p_.nets[static_cast<std::size_t>(q.net)].name) + ")\n";
        s += "\t\t\t(uuid " + quote(uuid()) + ")\n\t\t)\n";
      }
      s += "\t)\n";
    }
    const std::string stroke = "\t\t(stroke\n\t\t\t(width 0.05)\n\t\t\t(type default)\n\t\t)\n\t\t(fill no)\n\t\t(layer \"Edge.Cuts\")\n";
    if (p_.outline.empty()) {
      s += "\t(gr_rect\n\t\t(start " + format_mm(kx(p_.min_x)) + " " + format_mm(ky(p_.max_y)) + ")\n\t\t(end " + format_mm(kx(p_.max_x)) + " " + format_mm(ky(p_.min_y)) + ")\n";
    } else {
      s += "\t(gr_poly\n\t\t(pts\n\t\t\t";
      for (std::size_t i = 0; i < p_.outline.size(); ++i)
        s += std::string(i ? " " : "") + "(xy " + format_mm(kx(p_.outline[i].first)) + " " + format_mm(ky(p_.outline[i].second)) + ")";
      s += "\n\t\t)\n";
    }
    s += stroke + "\t\t(uuid " + quote(uuid()) + ")\n\t)\n";
    for (const auto& z : zones_) {
      s += "\t(zone\n";
      if (z.net >= 0) s += "\t\t(net " + quote(p_.nets[static_cast<std::size_t>(z.net)].name) + ")\n";
      std::vector<int> zl = z.layers;
      if (zl.empty())
        for (int k = 0; k < n; ++k) zl.push_back(k);
      s += "\t\t(layers " + layer_names(zl, false) + ")\n\t\t(uuid " + quote(uuid()) + ")\n\t\t(name " + quote(z.name) + ")\n\t\t(hatch edge 0.5)\n";
      s += "\t\t(connect_pads yes\n\t\t\t(clearance 0)\n\t\t)\n\t\t(min_thickness " + format_mm(mm_to_nm(p_.rules.min_trace_width)) + ")\n";
      if (z.keepout)
        s += std::string("\t\t(keepout\n\t\t\t(tracks not_allowed)\n\t\t\t(vias ") + (z.vias_ok ? "allowed" : "not_allowed") + ")\n\t\t\t(pads allowed)\n\t\t\t(copperpour not_allowed)\n\t\t\t(footprints allowed)\n\t\t)\n";
      s += "\t\t(fill\n\t\t\t(thermal_gap 0.5)\n\t\t\t(thermal_bridge_width 0.5)\n\t\t\t(island_removal_mode 0)\n\t\t)\n\t\t(polygon\n\t\t\t(pts\n\t\t\t\t";
      for (std::size_t i = 0; i < z.pts.size(); ++i) s += std::string(i ? " " : "") + "(xy " + format_mm(z.pts[i].first) + " " + format_mm(z.pts[i].second) + ")";
      s += "\n\t\t\t)\n\t\t)\n\t)\n";
    }
    s += "\t(embedded_fonts no)\n)\n";
    return s;
  }

  // Board minimums and net classes exactly as the problem states them; what it leaves out is zero (no rule), so
  // KiCad's judge is neither stricter nor laxer than the problem.
  std::string project_text() const {
    using nlohmann::ordered_json;
    const auto& r = p_.rules;
    auto mm = [](Coord v) { return static_cast<double>(v) / 1e6; };
    ordered_json rules;
    rules["min_clearance"] = mm(std::min({out_.clearance, pair_clearance(r.trace_to_pad), pair_clearance(r.via_to_pad)}));
    rules["min_track_width"] = r.min_trace_width;
    rules["min_via_diameter"] = mm(out_.via_diameter);
    rules["min_through_hole_diameter"] = mm(out_.via_drill);
    rules["min_via_annular_width"] = mm(std::max<Coord>(0, (out_.via_diameter - out_.via_drill) / 2));
    rules["min_hole_clearance"] = r.trace_to_hole.value_or(0.0);
    rules["min_hole_to_hole"] = mm(out_.hole_to_hole);
    rules["min_copper_edge_clearance"] = r.board_edge.value_or(0.0);
    rules["allow_blind_buried_vias"] = r.blind_buried_vias;
    rules["allow_microvias"] = false;
    auto net_class = [&](const std::string& name, Coord width, int priority) {
      ordered_json c;
      c["name"] = name;
      c["clearance"] = mm(out_.clearance);
      c["track_width"] = mm(width);
      c["via_diameter"] = mm(out_.via_diameter);
      c["via_drill"] = mm(out_.via_drill);
      c["priority"] = priority;
      return c;
    };
    ordered_json classes = ordered_json::array({net_class("Default", out_.track_width, 2147483647)});
    ordered_json patterns = ordered_json::array();
    std::map<Coord, std::string> by_width;
    for (std::size_t i = 0; i < p_.nets.size(); ++i)
      if (const Coord w = net_width(static_cast<int>(i)); w != out_.track_width) by_width.emplace(w, "SRJ_" + format_mm(w) + "mm");
    for (const auto& [w, name] : by_width) classes.push_back(net_class(name, w, 0));
    for (std::size_t i = 0; i < p_.nets.size(); ++i)
      if (const Coord w = net_width(static_cast<int>(i)); w != out_.track_width) patterns.push_back({{"netclass", by_width.at(w)}, {"pattern", p_.nets[i].name}});
    ordered_json d;
    d["board"]["design_settings"]["rules"] = rules;
    d["meta"] = {{"filename", "srj.kicad_pro"}, {"version", 3}};
    d["net_settings"]["classes"] = classes;
    d["net_settings"]["meta"] = {{"version", 4}};
    d["net_settings"]["netclass_patterns"] = patterns;
    return d.dump(2) + "\n";
  }

  Coord pair_clearance(const std::optional<double>& v) const { return v ? mm_to_nm(*v) : out_.clearance; }

  // Pad clearances that differ from the general one are custom rules: KiCad has no such board setting.
  std::string rules_text() const {
    const auto& r = p_.rules;
    std::string s;
    auto rule = [&](const char* name, const char* a, const std::optional<double>& v) {
      if (!v || mm_to_nm(*v) == out_.clearance) return;
      s += std::string("(rule \"") + name + "\"\n\t(condition \"A.Type == '" + a + "' && B.Type == 'Pad'\")\n\t(constraint clearance (min " + format_mm(mm_to_nm(*v)) + "mm))\n)\n";
    };
    rule("srj_trace_to_pad", "Track", r.trace_to_pad);
    rule("srj_via_to_pad", "Via", r.via_to_pad);
    return s.empty() ? s : "(version 1)\n" + s;
  }

  const SrjProblem& p_;
  const SrjImportOptions& opt_;
  SrjBoardFiles out_;
  std::vector<FootprintOut> fps_;
  std::map<std::string, std::size_t> fp_index_;
  std::vector<ZoneOut> zones_;
  std::uint64_t uuid_counter_ = 0;
};

}  // namespace

SrjBoardFiles srj_to_kicad(const SrjProblem& problem, const SrjImportOptions& opt) {
  if (opt.outline_margin == 0 && !opt.unpointed_keepout) return Writer(problem, opt).run();
  SrjProblem grown = problem;
  if (opt.unpointed_keepout) {
    for (auto& o : grown.obstacles) {
      if (o.net < 0) continue;
      bool pointed = false;
      for (int ci : grown.nets[static_cast<std::size_t>(o.net)].connections)
        for (const auto& pt : grown.connections[static_cast<std::size_t>(ci)].points)
          if (Writer::contains(o, pt.x, pt.y) && std::any_of(pt.layers.begin(), pt.layers.end(), [&](int z) { return std::find(o.layers.begin(), o.layers.end(), z) != o.layers.end(); })) pointed = true;
      if (!pointed) o.net = -1;
    }
    for (auto& nt : grown.nets) {
      nt.obstacles.erase(std::remove_if(nt.obstacles.begin(), nt.obstacles.end(), [&](int i) { return grown.obstacles[static_cast<std::size_t>(i)].net < 0; }), nt.obstacles.end());
    }
  }
  const double m = static_cast<double>(opt.outline_margin) / 1e6;
  grown.min_x -= m;
  grown.min_y -= m;
  grown.max_x += m;
  grown.max_y += m;
  return Writer(grown, opt).run();
}

}  // namespace tmk::io
