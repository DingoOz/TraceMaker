#include "place/problem.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

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
// must not become the board). Gaps in sloppy outlines are closed with growing tolerances (2 µm .. 0.5 mm).
void assemble_outline(Problem& p, const model::Board& b) {
  for (const Coord tol : {Coord{2'000}, Coord{50'000}, Coord{200'000}, Coord{500'000}}) {
    auto loops = edge_loops(p.edges, tol);
    std::size_t best = loops.size();
    for (std::size_t i = 0; i < loops.size(); ++i)
      if (best == loops.size() || loop_area(loops[i]) > loop_area(loops[best])) best = i;
    if (best == loops.size()) continue;
    std::size_t inside = 0;
    for (const auto& pd : b.pads) inside += geom::point_in_polygon(pd.pos, loops[best]) ? 1u : 0u;
    if (inside * 10 < b.pads.size() * 8u) continue;
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

Shape translated(const Shape& s, Point d) {
  Shape t = s;
  for (auto& q : t.pts) q = q + d;
  t.update_box();
  return t;
}

std::vector<Point> convex_hull(std::vector<Point> pts) {
  // Andrew's monotone chain (exact integer orientation tests).
  std::sort(pts.begin(), pts.end(), [](Point a, Point b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  if (pts.size() < 3) return pts;
  std::vector<Point> h(2 * pts.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    while (k >= 2 && geom::orient(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
    h[k++] = pts[i];
  }
  for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && geom::orient(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
    h[k++] = pts[i - 1];
  }
  h.resize(k - 1);
  return h;
}

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

bool power_like_name(const std::string& name) {
  std::string n = upper(name);
  if (const auto s = n.rfind('/'); s != std::string::npos) n = n.substr(s + 1);
  if (n.empty()) return false;
  if (n.find("GND") != std::string::npos || n.find("PWR") != std::string::npos) return true;
  if ((n[0] == '+' || n[0] == '-') && n.size() > 1 && (std::isdigit(static_cast<unsigned char>(n[1])) || n[1] == 'V')) return true;
  for (const char* pfx : {"VCC", "VDD", "VSS", "VEE", "VBUS", "VBAT", "VIN", "VSYS", "VPP", "VMOT", "V+", "V-", "AVCC", "AVDD", "DVDD", "VDDA", "VDDIO"})
    if (n.starts_with(pfx)) return true;
  // "3V3", "5V", "12V", "1V8": digits, a V, optional digits.
  std::size_t i = 0;
  while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
  if (i > 0 && i < n.size() && n[i] == 'V') {
    std::size_t j = i + 1;
    while (j < n.size() && std::isdigit(static_cast<unsigned char>(n[j]))) ++j;
    if (j == n.size()) return true;
  }
  return false;
}

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
    if (!h.plated && h.pad >= 0 && b.pads[z(h.pad)].footprint >= 0)
      fp_copper[z(b.pads[z(h.pad)].footprint)].push_back(CopperShape{h.shape, all_layers, 0, hole_need});
  for (const auto& t : b.texts) {
    const int l = b.copper_index(t.layer);
    if (l < 0 || t.hidden || t.text.empty()) continue;
    CopperShape cs{text_box(t), model::layer_bit(l), 0, net_need(0)};
    if (t.footprint >= 0) fp_copper[z(t.footprint)].push_back(std::move(cs));
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
      auto hull = convex_hull(cpts[z(s)]);
      if (hull.size() < 3) continue;
      for (auto& q : hull) q = q - fp.pos;
      cy0[z(s)].push_back(Shape::polygon(std::move(hull), 0));
    }
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
    for (int r = 0; r < 4; ++r) {
      PartGeom& g = pt.geom[z(r)];
      auto rot_shape = [&](const Shape& s) {
        Shape t = s;
        for (auto& q : t.pts) q = rot90(q, r);
        t.update_box();
        return t;
      };
      for (int s = 0; s < 2; ++s)
        for (const auto& sh : cy0[z(s)]) {
          g.cy[z(s)].push_back(rot_shape(sh));
          g.body.add(g.cy[z(s)].back().box);
          g.cy_in[z(s)].push_back(rot_shape(inset_convex(sh, kEdgeTolerance)));
          g.edge_box.add(g.cy_in[z(s)].back().box);
        }
      for (const auto& s : through) {
        g.through.push_back(rot_shape(s));
        g.body.add(g.through.back().box);
      }
      for (const auto& s : pads) {
        g.pads.push_back(rot_shape(s));
        g.edge_box.add(g.pads.back().box.inflated(p.edge_clearance));
      }
      for (const auto& cs : fp_copper[fi]) {
        g.copper.push_back(CopperShape{rot_shape(translated(cs.s, Point{} - fp.pos)), cs.layers, cs.net, cs.need});
        g.copper_box.add(g.copper.back().s.box);
      }
      g.body.add(g.copper_box);
    }
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
  std::vector<std::vector<std::pair<int, Point>>> net_pins(b.nets.size());  // (part, absolute pad position)
  for (const auto& pd : b.pads) {
    if (pd.net <= 0 || pd.footprint < 0) continue;
    const int pi = part_of_fp[z(pd.footprint)];
    if (pi < 0) continue;
    net_pins[z(pd.net)].emplace_back(pi, pd.pos);
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
    for (const auto& [part, abs] : v) {
      Pin pin;
      pin.part = part;
      pin.net = ni;
      const Point off = abs - p.parts[z(part)].pos0;
      for (int r = 0; r < 4; ++r) pin.off[z(r)] = rot90(off, r);
      net.pins.push_back(static_cast<int>(p.pins.size()));
      p.parts[z(part)].pins.push_back(static_cast<int>(p.pins.size()));
      p.pins.push_back(pin);
    }
    p.nets.push_back(std::move(net));
  }
  // Parts that already overhang the board edge (pads outside, or the courtyard well past it) or sit in a
  // keepout are placed that way on purpose (connectors, sensors, battery holders): keep them where they are.
  // Pads merely closer to the edge than the copper-to-edge clearance do not count (common in old boards).
  {
    const Legality L(p);
    for (auto& pt : p.parts)
      if (pt.movable && !L.inside_ok(static_cast<int>(&pt - p.parts.data()), pt.pos0, 0, true)) {
        pt.movable = false;
        pt.fixed_reason = "overhangs the board edge in the input";
      }
  }
  return p;
}

}  // namespace tmk::place
