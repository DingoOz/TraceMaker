// SPDX-License-Identifier: GPL-3.0-or-later
#include "place/problem.hpp"

#include <algorithm>
#include <optional>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include "crules/names.hpp"
#include "crules/topology.hpp"
#include "drc/copper.hpp"
#include "io/kicad/project_reader.hpp"
#include "place/legality.hpp"

namespace tmk::place {

namespace {

constexpr Coord kFallbackMargin = 250'000;  // courtyard fallback: pad bbox inflated by 0.25 mm
constexpr Coord kEdgeConnectorReach = 2'000'000;

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool starts_with_digit_after(const std::string& s, std::size_t n) {
  return s.size() > n && std::isdigit(static_cast<unsigned char>(s[n]));
}

bool connector_ref(const std::string& ref) {
  const std::string r = upper(ref);
  if ((r.starts_with("J") || r.starts_with("P")) && starts_with_digit_after(r, 1)) return true;
  return r.starts_with("CN") || r.starts_with("CON") || r.starts_with("USB") || r.starts_with("XS");
}

bool mounting_hole(const model::Board& b, const model::Footprint& fp) {
  const std::string r = upper(fp.reference);
  const bool ref_like = (r.starts_with("MH") || (r.starts_with("H") && starts_with_digit_after(r, 1)));
  bool netless = true;
  for (int pi : fp.pads)
    if (b.pads[z(pi)].net != 0) netless = false;
  return (ref_like && netless) || upper(fp.lib_id).find("MOUNTINGHOLE") != std::string::npos;
}

// Closed loops of Edge.Cuts pieces (as route/obstacles does), joining endpoints closer than `tol`: each step
// appends the piece whose nearest end is closest to the chain's end.
std::vector<std::vector<Point>> edge_loops(const std::vector<Shape>& edges, Coord tol) {
  std::vector<std::vector<Point>> pieces;
  for (const auto& e : edges) pieces.push_back(e.pts);
  auto dist = [](Point a, Point c) { return std::max(std::llabs(a.x - c.x), std::llabs(a.y - c.y)); };
  std::vector<std::uint8_t> used(pieces.size(), 0);
  std::vector<std::vector<Point>> loops;
  for (std::size_t s0 = 0; s0 < pieces.size(); ++s0) {
    if (used[s0] || pieces[s0].size() < 2) continue;
    used[s0] = 1;
    std::vector<Point> chain = pieces[s0];
    while (dist(chain.front(), chain.back()) > tol || chain.size() < 3) {
      std::size_t best = pieces.size();
      bool rev = false;
      Coord bd = tol + 1;
      for (std::size_t k = 0; k < pieces.size(); ++k) {
        if (used[k] || pieces[k].size() < 2) continue;
        if (const Coord d = dist(chain.back(), pieces[k].front()); d < bd) bd = d, best = k, rev = false;
        if (const Coord d = dist(chain.back(), pieces[k].back()); d < bd) bd = d, best = k, rev = true;
      }
      if (best == pieces.size()) break;
      used[best] = 1;
      if (rev) chain.insert(chain.end(), pieces[best].rbegin() + 1, pieces[best].rend());
      else chain.insert(chain.end(), pieces[best].begin() + 1, pieces[best].end());
    }
    if (chain.size() >= 4 && dist(chain.front(), chain.back()) <= tol) loops.push_back(std::move(chain));
  }
  return loops;
}

long double loop_area(const std::vector<Point>& l) {
  long double a = 0;
  for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++)
    a += static_cast<long double>(l[j].x) * static_cast<long double>(l[i].y) - static_cast<long double>(l[i].x) * static_cast<long double>(l[j].y);
  return std::fabs(a) / 2;
}

// The outline is the largest loop, accepted only if it contains most pad centres (a lone mounting-hole circle
// must not become the board). Ignore pads beyond the Edge.Cuts extents: new parts may sit beside the board.
// Gaps in sloppy outlines are closed with growing tolerances (2 µm .. 0.5 mm).
void assemble_outline(Problem& p, const model::Board& b) {
  const Box extents = b.edge_bbox();
  for (const Coord tol : {Coord{2'000}, Coord{50'000}, Coord{200'000}, Coord{500'000}}) {
    auto loops = edge_loops(p.edges, tol);
    std::size_t best = loops.size();
    for (std::size_t i = 0; i < loops.size(); ++i)
      if (best == loops.size() || loop_area(loops[i]) > loop_area(loops[best])) best = i;
    if (best == loops.size()) continue;
    std::size_t inside = 0, counted = 0;
    for (const auto& pd : b.pads) {
      if (!extents.empty() && !extents.intersects(Box{pd.pos.x, pd.pos.y, pd.pos.x, pd.pos.y})) continue;
      ++counted;
      inside += geom::point_in_polygon(pd.pos, loops[best]) ? 1u : 0u;
    }
    if (inside * 10 < counted * 8u) continue;
    p.outline = loops[best];
    for (std::size_t i = 0; i < loops.size(); ++i)
      if (i != best) p.cutouts.push_back(loops[i]);
    if (tol > 2'000) p.notes.push_back("board outline closed with " + std::to_string(nm_to_mm(tol)) + " mm gap tolerance");
    return;
  }
}

Coord rules_courtyard_clearance(const model::DesignRules& rules, const std::string& board_path, std::string& source) {
  Coord best = -1;
  for (const auto& r : rules.custom)
    for (const auto& c : r.constraints)
      if (c.type == "courtyard_clearance" && c.min && *c.min > best) {
        best = *c.min;
        source = "custom rule '" + r.name + "'";
      }
  if (best >= 0) return best;
  // A .kicad_pro next to the board may carry a courtyard rule in the design settings (any key naming it).
  std::string pro = board_path;
  if (const auto dot = pro.rfind(".kicad_pcb"); dot != std::string::npos) pro = pro.substr(0, dot) + ".kicad_pro";
  std::ifstream in(pro);
  if (!in) return -1;
  try {
    const auto j = nlohmann::json::parse(in, nullptr, true, true);
    const auto* rr = &j;
    for (const char* k : {"board", "design_settings", "rules"}) {
      if (!rr->contains(k)) return -1;
      rr = &(*rr)[k];
    }
    for (const auto& [k, v] : rr->items())
      if (k.find("courtyard") != std::string::npos && v.is_number()) {
        best = std::max(best, mm_to_nm(v.get<double>()));
        source = pro + " rules." + k;
      }
  } catch (const std::exception&) {
    return -1;
  }
  return best;
}

}  // namespace

int Problem::movable_count() const {
  int n = 0;
  for (const auto& pt : parts) n += pt.movable ? 1 : 0;
  return n;
}

Point rot90(Point p, int r) {
  switch (r & 3) {
    case 1: return {p.y, -p.x};
    case 2: return {-p.x, -p.y};
    case 3: return {-p.y, p.x};
    default: return p;
  }
}

int Problem::flippable_count() const {
  int n = 0;
  for (const auto& pt : parts) n += pt.flippable ? 1 : 0;
  return n;
}

Shape mirrored(const Shape& s) {
  Shape t = s;
  for (auto& q : t.pts) q = mirror_y(q);
  if (t.closed) std::reverse(t.pts.begin(), t.pts.end());  // keep the winding
  t.update_box();
  return t;
}

model::LayerMask flip_layers(model::LayerMask m, int n) {
  model::LayerMask out = 0;
  for (int i = 0; i < n && i < 64; ++i)
    if (m & model::layer_bit(i)) out |= model::layer_bit(n - 1 - i);
  return out;
}

Shape translated(const Shape& s, Point d) {
  Shape t = s;
  for (auto& q : t.pts) q = q + d;
  t.update_box();
  return t;
}

std::vector<Point> convex_hull(std::vector<Point> pts) { return geom::convex_hull(std::move(pts)); }

Shape inset_convex(const Shape& s, Coord t) {
  const std::size_t n = s.pts.size();
  if (n < 3 || t <= 0) return s;
  long double area2 = 0, cx = 0, cy = 0;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    area2 += static_cast<long double>(s.pts[j].x) * static_cast<long double>(s.pts[i].y) -
             static_cast<long double>(s.pts[i].x) * static_cast<long double>(s.pts[j].y);
    cx += static_cast<long double>(s.pts[i].x);
    cy += static_cast<long double>(s.pts[i].y);
  }
  const Point centre{static_cast<Coord>(cx / n), static_cast<Coord>(cy / n)};
  const double sgn = area2 > 0 ? 1.0 : -1.0;  // inward normal of edge a→b is sgn·(−dy, dx)
  std::vector<Point> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Point a = s.pts[(i + n - 1) % n], v = s.pts[i], b = s.pts[(i + 1) % n];
    auto normal = [&](Point p, Point q, double& nx, double& ny) {
      const double dx = static_cast<double>(q.x - p.x), dy = static_cast<double>(q.y - p.y);
      const double len = std::hypot(dx, dy);
      nx = len > 0 ? sgn * -dy / len : 0;
      ny = len > 0 ? sgn * dx / len : 0;
    };
    double n1x, n1y, n2x, n2y;
    normal(a, v, n1x, n1y);
    normal(v, b, n2x, n2y);
    const double k = 1.0 + n1x * n2x + n1y * n2y;
    if (k < 1e-6) return Shape::point(centre, 0);
    const double td = static_cast<double>(t);
    out[i] = Point{v.x + geom::kiround(td * (n1x + n2x) / k), v.y + geom::kiround(td * (n1y + n2y) / k)};
  }
  // Valid only if every edge keeps its direction (no edge collapsed or flipped).
  for (std::size_t i = 0; i < n; ++i) {
    const Point a = s.pts[i], b = s.pts[(i + 1) % n], c = out[i], d = out[(i + 1) % n];
    const long double dot = static_cast<long double>(b.x - a.x) * static_cast<long double>(d.x - c.x) +
                            static_cast<long double>(b.y - a.y) * static_cast<long double>(d.y - c.y);
    if (dot <= 0) return Shape::point(centre, 0);
  }
  return Shape::polygon(std::move(out), 0);
}

// Net-name conventions live in tm::crules (shared with component-rule detection, doc 15).
bool power_like_name(const std::string& name) { return crules::power_like_name(name); }
bool ground_like_name(const std::string& name) { return crules::ground_like_name(name); }

namespace {

// Conservative rectangle around copper text: any justification, stroke-font advance <= 1 glyph height,
// line pitch 1.62 heights, plus the stroke thickness.
Shape text_box(const model::Text& t) {
  std::size_t lines = 1, longest = 0, cur = 0;
  for (char ch : t.text) {
    if (ch == '\n') {
      ++lines;
      cur = 0;
      continue;
    }
    if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80) continue;
    longest = std::max(longest, ++cur);
  }
  const Coord hgt = std::max<Coord>({t.height, t.width, 300'000});
  const Coord th = std::max<Coord>(t.thickness, 150'000);
  const Coord W = static_cast<Coord>(longest) * hgt + th, H = static_cast<Coord>(static_cast<double>(lines) * 1.62 * static_cast<double>(hgt)) + th;
  std::vector<Point> pts = {{-W, -H}, {W, -H}, {W, H}, {-W, H}};
  for (auto& q : pts) q = t.pos + geom::rotate(q, t.angle);
  return Shape::polygon(std::move(pts), 0);
}

}  // namespace

Problem extract(const model::Board& b, const model::DesignRules& rules, const std::string& board_path, const ExtractOptions& opt) {
  Problem p;
  // Spacing rules.
  const std::string dflt = "default " + std::to_string(nm_to_mm(opt.default_clearance)) + " mm";
  std::string src = dflt;
  Coord cc = opt.courtyard_clearance;
  p.clearance_is_default = false;
  if (cc < 0) {
    cc = rules_courtyard_clearance(rules, board_path, src);
    if (cc < 0) {
      cc = opt.default_clearance;
      src = dflt;
      p.clearance_is_default = true;
    }
  } else {
    src = "command line";
  }
  p.clearance = cc;
  p.edge_clearance = std::max<Coord>(rules.minimums.copper_edge_clearance, 0);
  p.notes.push_back("courtyard clearance " + std::to_string(nm_to_mm(cc)) + " mm (" + src + ")");

  // Board outline.
  const drc::CopperModel cm = drc::build_copper(b);
  p.edges = cm.edges;

  // Copper clearances: net class (or board minimum), local overrides; with custom clearance rules, their
  // largest minimum as an upper bound (conditions are not evaluated here: conservative).
  p.copper_layers = std::max(1, b.copper_count());
  Coord custom_max = 0;
  for (const auto& r : rules.custom)
    for (const auto& k : r.constraints)
      if (k.type == "clearance" && k.min) custom_max = std::max(custom_max, *k.min);
  if (custom_max > 0) p.notes.push_back("custom clearance rules: placement uses their largest minimum for every pad (conservative)");
  auto net_need = [&](model::NetId net) {
    const std::string& name = net > 0 && static_cast<std::size_t>(net) < b.nets.size() ? b.nets[z(net)].name : std::string();
    return std::max({rules.class_for(name).clearance, rules.minimums.clearance, custom_max});
  };
  const model::LayerMask all_layers = p.copper_layers >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << p.copper_layers) - 1;
  const Coord hole_need = std::max({rules.minimums.hole_clearance, rules.default_class().clearance, custom_max});
  std::vector<std::vector<CopperShape>> fp_copper(b.footprints.size());
  for (const auto& it : cm.items) {
    if (it.kind == drc::ItemKind::Zone) continue;  // zone fills are regenerated around the new placement
    Coord need = net_need(it.net);
    if (it.kind == drc::ItemKind::Pad) {
      const auto& pd = b.pads[z(it.index)];
      if (pd.clearance >= 0) need = std::max(need, pd.clearance);
      if (pd.footprint >= 0 && b.footprints[z(pd.footprint)].clearance >= 0) need = std::max(need, b.footprints[z(pd.footprint)].clearance);
      // Solder-mask apertures of pads on different nets must not merge (KiCad solder_mask_bridge): the
      // copper gap must be at least the sum of both expansions, so each pad asks for twice its own.
      bool masked = false;
      for (const auto& l : pd.layers) masked |= l == "F.Mask" || l == "B.Mask" || l == "*.Mask" || l == "F&B.Mask";
      if (masked) {
        Coord exp = b.pad_to_mask_clearance;
        if (pd.footprint >= 0 && b.footprints[z(pd.footprint)].mask_margin != INT64_MIN) exp = b.footprints[z(pd.footprint)].mask_margin;
        if (pd.mask_margin != INT64_MIN) exp = pd.mask_margin;
        need = std::max(need, 2 * exp + std::max({rules.minimums.solder_mask_to_copper_clearance, rules.minimums.solder_mask_min_width, Coord{0}}));
      }
    }
    for (const auto& s : it.shapes) {
      CopperShape cs{s, it.layers, it.net, need};
      if (it.footprint >= 0) fp_copper[z(it.footprint)].push_back(std::move(cs));
      else p.fixed_copper.push_back(std::move(cs));
    }
  }
  for (const auto& h : cm.holes)
    if (!h.plated && h.pad >= 0 && b.pads[z(h.pad)].footprint >= 0) {
      // KiCad's hole_clearance check uses the NPTH pad's (or its footprint's) local clearance override too: a
      // mounting hole asking for 4.3 mm keeps every pad that far away.
      const auto& pd = b.pads[z(h.pad)];
      const auto& fp = b.footprints[z(pd.footprint)];
      const Coord need = std::max({hole_need, pd.clearance, fp.clearance});
      fp_copper[z(pd.footprint)].push_back(CopperShape{h.shape, all_layers, 0, need});
    }
  for (const auto& t : b.texts) {
    const int l = b.copper_index(t.layer);
    if (l < 0 || t.hidden || t.text.empty()) continue;
    CopperShape cs{text_box(t), model::layer_bit(l), 0, net_need(0)};
    if (t.footprint >= 0) fp_copper[z(t.footprint)].push_back(std::move(cs));
    else p.fixed_copper.push_back(std::move(cs));
  }
  // Solder-mask openings drawn as graphics (logos, bare-copper art, board-level openings): a pad whose own opening
  // reaches one is a KiCad solder_mask_bridge (PCBench kitspace_postcard: LEDs placed under a logo's opening). They
  // act as net-less copper on that side that pads keep the mask expansion away from. A footprint's graphic over
  // its own pads is those pads' opening and is left out.
  for (const auto& g : b.graphics) {
    const int side = g.layer == "F.Mask" ? 0 : g.layer == "B.Mask" ? 1 : -1;
    if (side < 0 || p.copper_layers < 1) continue;
    std::optional<Shape> sh;
    if ((g.kind == model::Graphic::Kind::Poly || g.kind == model::Graphic::Kind::Rect) && g.pts.size() >= 3 && (g.filled || g.kind == model::Graphic::Kind::Poly)) {
      // The convex hull: logo outlines have thousands of points, and every candidate position of every part is
      // tested against them (1Bitsy: full mode 2 s -> 43 s with the exact outlines). The hull keeps pads out of
      // a little more than the opening, never less.
      auto hull = geom::convex_hull(g.pts);
      if (hull.size() >= 3) sh = Shape::polygon(std::move(hull), g.width / 2);
    } else if (g.kind == model::Graphic::Kind::Circle && g.filled) {
      sh = Shape::point(g.a, geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y))) + g.width / 2);
    } else if (g.kind == model::Graphic::Kind::Line) {
      sh = Shape::segment(g.a, g.b, g.width / 2);
    }
    if (!sh) continue;
    bool own_pad = false;
    if (g.footprint >= 0)
      for (int pi : b.footprints[z(g.footprint)].pads)
        for (const auto& ps : drc::pad_shapes(b.pads[z(pi)]))
          if (geom::closer_than(*sh, ps, 1)) own_pad = true;
    if (own_pad) continue;
    const Coord need = std::max<Coord>(b.pad_to_mask_clearance, 0) + std::max({rules.minimums.solder_mask_min_width, Coord{0}}) + 1'000;
    CopperShape cs{*sh, model::layer_bit(side == 0 ? 0 : p.copper_layers - 1), 0, need};
    if (g.footprint >= 0) fp_copper[z(g.footprint)].push_back(std::move(cs));
    else p.fixed_copper.push_back(std::move(cs));
  }
  for (const auto& v : fp_copper)
    for (const auto& cs : v) p.max_need = std::max(p.max_need, cs.need);
  for (const auto& cs : p.fixed_copper) p.max_need = std::max(p.max_need, cs.need);
  assemble_outline(p, b);
  if (!p.outline.empty()) {
    for (const auto& q : p.outline) p.region.add(q);
  } else {
    p.region = b.edge_bbox();
    p.notes.push_back(p.region.empty() ? "no Edge.Cuts: parts are kept inside the footprint bounding box"
                                       : "Edge.Cuts do not form a closed loop around the parts: using their bounding box");
    // Closed Edge.Cuts loops inside the box (holes, slots) still exclude parts.
    for (auto& l : edge_loops(p.edges, 2'000)) p.cutouts.push_back(std::move(l));
  }
  if (p.region.empty()) {
    for (const auto& pd : b.pads) p.region.add(pd.pos);
    p.region = p.region.inflated(5'000'000);
  }

  // Footprints with copper Edge.Cuts or keepout zones of their own stay fixed (moving them would move the
  // board edge or a keepout).
  std::vector<std::uint8_t> owns_edges(b.footprints.size(), 0), owns_keepout(b.footprints.size(), 0);
  for (const auto& g : b.graphics)
    if (g.footprint >= 0 && g.layer == "Edge.Cuts") owns_edges[z(g.footprint)] = 1;
  for (const auto& zn : b.zones)
    if (zn.footprint >= 0 && zn.rule_area) owns_keepout[z(zn.footprint)] = 1;

  // Keepouts that disallow footprints (board-level).
  for (const auto& zn : b.zones) {
    if (!zn.rule_area || !zn.keepout_footprints || zn.outline.empty() || zn.outline.front().size() < 3) continue;
    Keepout k;
    k.poly = Shape::polygon(zn.outline.front(), 0);
    for (const auto& l : zn.layers) {
      if (l == "F.Cu" || l == "*.Cu" || l == "F&B.Cu") k.side[0] = true;
      if (l == "B.Cu" || l == "*.Cu" || l == "F&B.Cu") k.side[1] = true;
    }
    if (k.side[0] || k.side[1]) p.keepouts.push_back(std::move(k));
  }

  // Pads with routed copper on them pin their footprint (moving would break the routing).
  std::vector<Point> track_ends;
  for (const auto& t : b.tracks) {
    track_ends.push_back(t.a);
    track_ends.push_back(t.b);
  }
  for (const auto& a : b.arcs) {
    track_ends.push_back(a.a);
    track_ends.push_back(a.b);
  }
  for (const auto& v : b.vias) track_ends.push_back(v.pos);
  std::sort(track_ends.begin(), track_ends.end(), [](Point a, Point c) { return a.x != c.x ? a.x < c.x : a.y < c.y; });

  // Parts.
  std::vector<int> part_of_fp(b.footprints.size(), -1);
  std::vector<std::string> no_courtyard;
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const auto& fp = b.footprints[fi];
    Part pt;
    pt.fp = static_cast<int>(fi);
    pt.ref = fp.reference;
    pt.lib_id = fp.lib_id;
    pt.side = fp.back ? 1 : 0;
    pt.pos0 = fp.pos;
    pt.angle0 = fp.angle;

    // Courtyards: convex hull of every courtyard graphic per side (a superset of KiCad's courtyard polygon,
    // so a placement legal here is legal in KiCad).
    std::array<std::vector<Point>, 2> cpts;
    for (int gi : fp.graphics) {
      const auto& g = b.graphics[z(gi)];
      const int side = g.layer == "F.CrtYd" ? 0 : g.layer == "B.CrtYd" ? 1 : -1;
      if (side < 0) continue;
      auto& v = cpts[z(side)];
      switch (g.kind) {
        case model::Graphic::Kind::Line: v.push_back(g.a); v.push_back(g.b); break;
        case model::Graphic::Kind::Arc: {
          const auto a = geom::arc_points(g.a, g.c, g.b, 5'000);
          v.insert(v.end(), a.begin(), a.end());
          break;
        }
        case model::Graphic::Kind::Circle: {
          const Coord rad = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y)));
          const auto c = geom::circle_points(g.a, rad, 5'000);
          v.insert(v.end(), c.begin(), c.end());
          break;
        }
        default: v.insert(v.end(), g.pts.begin(), g.pts.end()); break;
      }
    }
    // Pad copper and through obstacles.
    std::vector<Shape> pads, through;
    std::vector<Box> pad_boxes;
    Box pad_box;
    for (int pi : fp.pads) {
      const auto& pd = b.pads[z(pi)];
      Box one;
      for (auto s : drc::pad_shapes(pd)) {
        one.add(s.box);
        if (pd.copper != 0) pads.push_back(translated(s, Point{} - fp.pos));
        if (pd.drill_x > 0 && pd.type == model::PadType::ThruHole) through.push_back(translated(s, Point{} - fp.pos));
      }
      if (pd.drill_x > 0) {
        const Coord r = std::max(pd.drill_x, pd.drill_y) / 2;
        through.push_back(Shape::point(pd.pos - fp.pos, r));
        one.add(Box{pd.pos.x - r, pd.pos.y - r, pd.pos.x + r, pd.pos.y + r});
      }
      if (!one.empty()) pad_boxes.push_back(one);
      pad_box.add(one);
    }
    // Courtyard shapes per side (offsets from the origin).
    std::array<std::vector<Shape>, 2> cy0;
    for (int s = 0; s < 2; ++s) {
      if (cpts[z(s)].size() < 3) continue;
      auto hull = geom::convex_hull(cpts[z(s)]);
      if (hull.size() < 3) continue;
      for (auto& q : hull) q = q - fp.pos;
      cy0[z(s)].push_back(Shape::polygon(std::move(hull), 0));
    }
    if (!pad_box.empty() && std::find(opt.pads_only.begin(), opt.pads_only.end(), fp.reference) != opt.pads_only.end()) cy0 = {};
    pt.copper_only = std::find(opt.copper_only.begin(), opt.copper_only.end(), fp.reference) != opt.copper_only.end();
    if (cy0[0].empty() && cy0[1].empty() && !pad_box.empty()) {
      // No courtyard: the pad bounding box inflated by 0.25 mm, or for large sparse footprints (shield headers,
      // board outlines drawn as footprints: pads cover < 20% of the box) one such box per pad, so the empty
      // middle stays usable.
      auto rect = [&](const Box& bx) {
        const Box f = bx.inflated(kFallbackMargin);
        return Shape::polygon({Point{f.x0, f.y0} - fp.pos, Point{f.x1, f.y0} - fp.pos, Point{f.x1, f.y1} - fp.pos, Point{f.x0, f.y1} - fp.pos}, 0);
      };
      const Box fb = pad_box.inflated(kFallbackMargin);
      const long double bb_area = static_cast<long double>(fb.x1 - fb.x0) * static_cast<long double>(fb.y1 - fb.y0);
      long double pads_area = 0;
      for (const auto& bx : pad_boxes) {
        const Box f = bx.inflated(kFallbackMargin);
        pads_area += static_cast<long double>(f.x1 - f.x0) * static_cast<long double>(f.y1 - f.y0);
      }
      if (bb_area <= 100e12L || pads_area >= 0.2L * bb_area) cy0[z(pt.side)].push_back(rect(pad_box));
      else
        for (const auto& bx : pad_boxes) cy0[z(pt.side)].push_back(rect(bx));
      no_courtyard.push_back(fp.reference);
    }
    if (cy0[0].empty() && cy0[1].empty()) {
      // Nothing to place (logos, art, net ties without pads), but its copper stays where it is and every moved
      // part must keep clear of it (PCBench komputer-klavier: parts were placed onto a copper logo).
      for (const auto& cs : fp_copper[fi]) p.fixed_copper.push_back(cs);
      continue;
    }

    long double cy_area = 0;
    for (const auto& side : cy0)
      for (const auto& s : side) cy_area += static_cast<long double>(s.box.x1 - s.box.x0 + cc) * static_cast<long double>(s.box.y1 - s.box.y0 + cc);
    // Flip candidates (D48): surface mount only (no drilled hole: a through obstacle is the same on both sides, but
    // KiCad's THT parts stay on top by default, doc 04 §2), two copper sides, and a footprint the writer can mirror.
    const bool flip_candidate = opt.flip && through.empty() && p.copper_layers >= 2 && (opt.flip_ok.empty() || opt.flip_ok[fi] != 0);
    for (int state = 0; state < (flip_candidate ? kStates : 4); ++state) {
      const bool fl = flipped(state);
      const int r = state & 3;
      PartGeom& g = pt.geom[z(state)];
      auto pre = [&](const Shape& s) { return fl ? mirrored(s) : s; };
      auto rot_shape = [&](const Shape& s) {
        Shape t = s;
        for (auto& q : t.pts) q = rot90(q, r);
        t.update_box();
        return t;
      };
      for (int s = 0; s < 2; ++s) {
        const int to = fl ? 1 - s : s;  // a flipped part's front courtyard is on the back
        for (const auto& sh : cy0[z(s)]) {
          g.cy[z(to)].push_back(rot_shape(pre(sh)));
          g.body.add(g.cy[z(to)].back().box);
          g.cy_in[z(to)].push_back(rot_shape(inset_convex(pre(sh), kEdgeTolerance)));
          g.edge_box.add(g.cy_in[z(to)].back().box);
        }
      }
      for (const auto& s : through) {
        g.through.push_back(rot_shape(pre(s)));
        g.body.add(g.through.back().box);
      }
      for (const auto& s : pads) {
        g.pads.push_back(rot_shape(pre(s)));
        g.edge_box.add(g.pads.back().box.inflated(p.edge_clearance));
      }
      for (const auto& cs : fp_copper[fi]) {
        g.copper.push_back(CopperShape{rot_shape(pre(translated(cs.s, Point{} - fp.pos))), fl ? flip_layers(cs.layers, p.copper_layers) : cs.layers,
                                       cs.net, cs.need});
        g.copper_box.add(g.copper.back().s.box);
      }
      g.body.add(g.copper_box);
    }
    if (flip_candidate) pt.flippable = true;  // confirmed below once movability is final
    else if (opt.flip) pt.flip_reason = !through.empty() ? "through-hole or drilled" : p.copper_layers < 2 ? "single copper layer"
                                       : opt.flip_why.size() > fi && !opt.flip_why[fi].empty() ? opt.flip_why[fi] : "footprint cannot be mirrored";
    pt.area = static_cast<Coord>(std::min<long double>(cy_area, 4e18L));
    pt.shape_key = std::hash<std::string>{}(fp.lib_id) * 31u + static_cast<std::uint64_t>(pt.side);

    // Movability.
    if (fp.locked) pt.fixed_reason = "locked";
    else if (fp.board_only) pt.fixed_reason = "board only";
    else if (fp.pads.empty()) pt.fixed_reason = "no pads";
    else if (fp.reference.starts_with("REF**")) pt.fixed_reason = "unannotated (REF**)";
    else if (fp.pads.size() == 1) pt.fixed_reason = "single pad (via, test point, fiducial)";
    else if (mounting_hole(b, fp)) pt.fixed_reason = "mounting hole";
    else if (owns_edges[fi]) pt.fixed_reason = "has Edge.Cuts";
    else if (owns_keepout[fi]) pt.fixed_reason = "has a keepout";
    if (pt.fixed_reason.empty() && !track_ends.empty()) {
      for (int pi : fp.pads) {
        Box bx;
        for (const auto& s : drc::pad_shapes(b.pads[z(pi)])) bx.add(s.box);
        if (bx.empty()) continue;
        auto it = std::lower_bound(track_ends.begin(), track_ends.end(), Point{bx.x0, INT64_MIN},
                                   [](Point a, Point c) { return a.x != c.x ? a.x < c.x : a.y < c.y; });
        for (; it != track_ends.end() && it->x <= bx.x1; ++it)
          if (it->y >= bx.y0 && it->y <= bx.y1) {
            pt.fixed_reason = "routed";
            break;
          }
        if (!pt.fixed_reason.empty()) break;
      }
    }
    if (pt.fixed_reason.empty() && opt.fix_edge_connectors && connector_ref(fp.reference)) {
      for (int s = 0; s < 2 && pt.fixed_reason.empty(); ++s) {
        for (const auto& sh : pt.geom[0].cy[z(s)]) {
          const Shape cy = translated(sh, fp.pos);
          for (const auto& e : p.edges)
            if (geom::closer_than(cy, e, kEdgeConnectorReach)) {
              pt.fixed_reason = "edge connector";
              break;
            }
        }
      }
    }
    pt.movable = pt.fixed_reason.empty();
    part_of_fp[fi] = static_cast<int>(p.parts.size());
    p.parts.push_back(std::move(pt));
  }

  if (!no_courtyard.empty()) {
    std::string s = std::to_string(no_courtyard.size()) + " footprint(s) without courtyard, using pad boxes + 0.25 mm:";
    for (std::size_t k = 0; k < no_courtyard.size() && k < 12; ++k) s += " " + no_courtyard[k];
    if (no_courtyard.size() > 12) s += " ...";
    p.notes.push_back(s);
  }

  // Nets and pins.
  struct NetPin {
    int first;   // part
    Point pos;   // absolute pad position
    bool smd;    // surface mount: copper on one side only
  };
  std::vector<std::vector<NetPin>> net_pins(b.nets.size());
  for (const auto& pd : b.pads) {
    if (pd.net <= 0 || pd.footprint < 0) continue;
    const int pi = part_of_fp[z(pd.footprint)];
    if (pi < 0) continue;
    const bool smd = pd.drill_x == 0 && (pd.type == model::PadType::Smd || pd.type == model::PadType::Connect);
    net_pins[z(pd.net)].push_back(NetPin{pi, pd.pos, smd});
  }
  for (std::size_t n = 1; n < b.nets.size(); ++n) {
    const auto& v = net_pins[n];
    if (v.size() < 2) continue;
    bool multi = false;
    for (const auto& q : v) multi |= q.first != v.front().first;
    if (!multi) continue;  // all pins on one part: constant wirelength
    PNet net;
    net.name = b.nets[n].name;
    const bool power = power_like_name(net.name) || v.size() > 30;
    net.weight = power ? kPowerWeight : kSignalWeight;
    net.signal = !power;
    const int ni = static_cast<int>(p.nets.size());
    for (const auto& [part, abs, smd] : v) {
      Pin pin;
      pin.part = part;
      pin.net = ni;
      pin.one_side = smd;
      const Point off = abs - p.parts[z(part)].pos0;
      for (int r = 0; r < 4; ++r) pin.off[z(r)] = rot90(off, r);
      net.pins.push_back(static_cast<int>(p.pins.size()));
      p.parts[z(part)].pins.push_back(static_cast<int>(p.pins.size()));
      p.pins.push_back(pin);
    }
    p.nets.push_back(std::move(net));
  }
  // Decoupling capacitors (design doc 04 §2, decision D25): a two-pad capacitor between a supply and a ground sits only on
  // power nets, which carry weight 1, so wirelength alone lets it drift away from the IC it decouples. Tie its
  // supply pad to the nearest pad of an IC (U*, IC*) on the same supply in the input placement, at signal weight.
  // A two-pin pseudo-net between two pads: objective only (never reported as wirelength, never routed, never
  // counted for crossings or congestion), so the reported HPWL is unchanged.
  auto add_affinity = [&](int pad_a, int pad_b, int weight, std::string name) {
    PNet net;
    net.name = std::move(name);
    net.weight = weight;
    net.signal = false;
    net.affinity = true;
    const int ni = static_cast<int>(p.nets.size());
    for (const int pi : {pad_a, pad_b}) {
      const model::Pad& q = b.pads[z(pi)];
      Pin pin;
      pin.part = part_of_fp[z(q.footprint)];
      pin.net = ni;
      const Point off = q.pos - p.parts[z(pin.part)].pos0;
      for (int rr = 0; rr < 4; ++rr) pin.off[z(rr)] = rot90(off, rr);
      net.pins.push_back(static_cast<int>(p.pins.size()));
      p.parts[z(pin.part)].pins.push_back(static_cast<int>(p.pins.size()));
      p.pins.push_back(pin);
    }
    p.nets.push_back(std::move(net));
  };
  std::vector<std::uint8_t> tied_fp(b.footprints.size(), 0);
  if (opt.decap_affinity) {
    int tied = 0;
    for (const auto& t : crules::decap_ties(b, [&](int fi) { return part_of_fp[z(fi)] >= 0; }, false)) {
      add_affinity(t.cap_pad, t.ic_pad, opt.decap_weight,
                   "~decap " + b.footprints[z(t.cap_fp)].reference + "-" + b.footprints[z(b.pads[z(t.ic_pad)].footprint)].reference);
      tied_fp[z(t.cap_fp)] = 1;
      ++tied;
    }
    if (tied > 0) p.notes.push_back(std::to_string(tied) + " decoupling capacitor(s) tied to the nearest supply pin of their IC");
  }
  // Component-rule proximity pseudo-nets (doc 15 P1), the same mechanism as D25. A part D25 already tied keeps
  // only its D25 tie, so the default decoupling behaviour does not change.
  if (!opt.affinities.empty()) {
    int added = 0, skipped = 0;
    for (const auto& a : opt.affinities) {
      if (a.pad_a < 0 || a.pad_b < 0 || static_cast<std::size_t>(a.pad_a) >= b.pads.size() || static_cast<std::size_t>(a.pad_b) >= b.pads.size()) continue;
      const int fa = b.pads[z(a.pad_a)].footprint, fb = b.pads[z(a.pad_b)].footprint;
      if (fa < 0 || fb < 0 || fa == fb || part_of_fp[z(fa)] < 0 || part_of_fp[z(fb)] < 0) continue;
      if (tied_fp[z(fa)]) {
        ++skipped;
        continue;
      }
      add_affinity(a.pad_a, a.pad_b, std::max(1, a.weight * opt.crules_weight_pct / 100), a.name);
      ++added;
    }
    p.notes.push_back("component rules: " + std::to_string(added) + " proximity pseudo-net(s)" +
                      (skipped ? ", " + std::to_string(skipped) + " skipped (part already tied as a decoupling capacitor)" : std::string()));
  }
  // Parts that already overhang the board edge (pads outside, or the courtyard well past it) or sit in a
  // keepout are placed that way on purpose (connectors, sensors, battery holders): keep them where they are.
  // Pads merely closer to the edge than the copper-to-edge clearance do not count (common in old boards).
  // KiCad drops new parts beside the board; parts entirely clear of its bounding box stay movable.
  if (!opt.scratch) {
    const Legality L(p);
    for (auto& pt : p.parts) {
      const Box& body = pt.geom[0].body;
      const Box at{pt.pos0.x + body.x0, pt.pos0.y + body.y0, pt.pos0.x + body.x1, pt.pos0.y + body.y1};
      if (!at.intersects(p.region)) continue;
      if (pt.movable && !L.inside_ok(static_cast<int>(&pt - p.parts.data()), pt.pos0, 0, true)) {
        pt.movable = false;
        pt.fixed_reason = "overhangs the board edge in the input";
      }
    }
  }
  // Edge pulls (doc 15 CONN-01, opt-in): after movability is final, so fixed parts (edge connectors already at the
  // edge, locked parts) get none.
  if (!opt.edge_pulls.empty()) {
    int added = 0, fixed = 0;
    for (const auto& e : opt.edge_pulls) {
      if (e.footprint < 0 || static_cast<std::size_t>(e.footprint) >= b.footprints.size() || e.body.empty()) continue;
      const int part = part_of_fp[z(e.footprint)];
      if (part < 0 || !p.parts[z(part)].movable) {
        ++fixed;
        continue;
      }
      if (p.outline.size() < 3) break;
      // The body point nearest to the outer outline (cut-outs do not count) and the segment it is nearest to;
      // ties keep the first point and segment (input order).
      double best = 0;
      Point from{}, at{};
      bool vertical = false, found = false;
      for (const Point& q : e.body)
        for (std::size_t i = 0; i < p.outline.size(); ++i) {
          const Point a = p.outline[i], c = p.outline[(i + 1) % p.outline.size()];
          const double dx = static_cast<double>(c.x - a.x), dy = static_cast<double>(c.y - a.y);
          const double len2 = dx * dx + dy * dy;
          double t = len2 > 0 ? (static_cast<double>(q.x - a.x) * dx + static_cast<double>(q.y - a.y) * dy) / len2 : 0.0;
          t = std::clamp(t, 0.0, 1.0);
          const Point foot{a.x + geom::kiround(t * dx), a.y + geom::kiround(t * dy)};
          const double d = std::hypot(static_cast<double>(q.x - foot.x), static_cast<double>(q.y - foot.y));
          if (!found || d < best) found = true, best = d, from = q, at = foot, vertical = std::fabs(dy) >= std::fabs(dx);
        }
      PNet net;
      net.name = e.name;
      net.weight = std::max(1, e.weight * opt.crules_weight_pct / 100);
      net.signal = false;
      net.affinity = true;
      if (vertical) net.has_ax = true, net.ax = at.x;
      else net.has_ay = true, net.ay = at.y;
      const int ni = static_cast<int>(p.nets.size());
      Pin pin;
      pin.part = part;
      pin.net = ni;
      const Point off = from - p.parts[z(part)].pos0;
      for (int rr = 0; rr < 4; ++rr) pin.off[z(rr)] = rot90(off, rr);
      net.pins.push_back(static_cast<int>(p.pins.size()));
      p.parts[z(part)].pins.push_back(static_cast<int>(p.pins.size()));
      p.pins.push_back(pin);
      p.nets.push_back(std::move(net));
      ++added;
    }
    p.notes.push_back("component rules: " + std::to_string(added) + " connector edge pull(s)" +
                      (fixed ? ", " + std::to_string(fixed) + " connector(s) fixed (not pulled)" : std::string()) +
                      (p.outline.size() < 3 ? "; no closed board outline: no pulls" : std::string()));
  }
  // Flipped states: pin offsets mirrored in y, then turned (see kStates).
  for (auto& q : p.pins)
    for (int r = 0; r < 4; ++r) q.off[z(4 + r)] = rot90(mirror_y(q.off[0]), r);
  // Side assignment: only parts that may move at all, never parts the user pinned to their side (rule 6).
  if (opt.flip) {
    int n = 0;
    for (auto& pt : p.parts) {
      if (!pt.movable) {
        pt.flippable = false;
        pt.flip_reason.clear();
        continue;
      }
      if (pt.flippable && std::find(opt.keep_side.begin(), opt.keep_side.end(), pt.ref) != opt.keep_side.end()) {
        pt.flippable = false;
        pt.flip_reason = "--keep-side";
      }
      n += pt.flippable ? 1 : 0;
    }
    p.notes.push_back("side assignment: " + std::to_string(n) + " of " + std::to_string(p.movable_count()) + " movable part(s) may flip");
  }
  return p;
}

}  // namespace tmk::place
