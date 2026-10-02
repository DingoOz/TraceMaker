#include "place/problem.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include "drc/copper.hpp"
#include "io/kicad/project_reader.hpp"

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

// Largest closed loop of Edge.Cuts pieces is the outline, other loops are cut-outs (same as route/obstacles).
void assemble_outline(Problem& p) {
  std::vector<std::vector<Point>> pieces;
  for (const auto& e : p.edges) pieces.push_back(e.pts);
  auto near = [](Point a, Point c) { return std::llabs(a.x - c.x) < 2000 && std::llabs(a.y - c.y) < 2000; };
  std::vector<std::uint8_t> used(pieces.size(), 0);
  std::vector<std::vector<Point>> loops;
  for (std::size_t s0 = 0; s0 < pieces.size(); ++s0) {
    if (used[s0] || pieces[s0].size() < 2) continue;
    used[s0] = 1;
    std::vector<Point> chain = pieces[s0];
    for (bool grown = true; grown && !near(chain.front(), chain.back());) {
      grown = false;
      for (std::size_t k = 0; k < pieces.size(); ++k) {
        if (used[k] || pieces[k].size() < 2) continue;
        if (near(chain.back(), pieces[k].front())) chain.insert(chain.end(), pieces[k].begin() + 1, pieces[k].end());
        else if (near(chain.back(), pieces[k].back())) chain.insert(chain.end(), pieces[k].rbegin() + 1, pieces[k].rend());
        else continue;
        used[k] = 1;
        grown = true;
        break;
      }
    }
    if (chain.size() >= 4 && near(chain.front(), chain.back())) loops.push_back(std::move(chain));
  }
  auto area = [](const std::vector<Point>& l) {
    long double a = 0;
    for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++)
      a += static_cast<long double>(l[j].x) * static_cast<long double>(l[i].y) - static_cast<long double>(l[i].x) * static_cast<long double>(l[j].y);
    return std::fabs(a) / 2;
  };
  std::size_t best = loops.size();
  for (std::size_t i = 0; i < loops.size(); ++i)
    if (best == loops.size() || area(loops[i]) > area(loops[best])) best = i;
  if (best == loops.size()) return;
  p.outline = loops[best];
  for (std::size_t i = 0; i < loops.size(); ++i)
    if (i != best) p.cutouts.push_back(loops[i]);
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

Problem extract(const model::Board& b, const model::DesignRules& rules, const std::string& board_path, const ExtractOptions& opt) {
  Problem p;
  // Spacing rules.
  std::string src = "default 0.25 mm";
  Coord cc = opt.courtyard_clearance;
  if (cc < 0) {
    cc = rules_courtyard_clearance(rules, board_path, src);
    if (cc < 0) {
      cc = 250'000;
      src = "default 0.25 mm";
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
  assemble_outline(p);
  if (!p.outline.empty()) {
    for (const auto& q : p.outline) p.region.add(q);
  } else {
    p.region = b.edge_bbox();
    p.notes.push_back(p.region.empty() ? "no Edge.Cuts: parts are kept inside the footprint bounding box"
                                       : "Edge.Cuts do not form a closed loop: using its bounding box");
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
    Box pad_box;
    for (int pi : fp.pads) {
      const auto& pd = b.pads[z(pi)];
      for (auto s : drc::pad_shapes(pd)) {
        pad_box.add(s.box);
        if (pd.copper != 0) pads.push_back(translated(s, Point{} - fp.pos));
        if (pd.drill_x > 0 && pd.type == model::PadType::ThruHole) through.push_back(translated(s, Point{} - fp.pos));
      }
      if (pd.drill_x > 0) {
        const Coord r = std::max(pd.drill_x, pd.drill_y) / 2;
        through.push_back(Shape::point(pd.pos - fp.pos, r));
        pad_box.add(Box{pd.pos.x - r, pd.pos.y - r, pd.pos.x + r, pd.pos.y + r});
      }
    }
    if (cpts[0].empty() && cpts[1].empty() && !pad_box.empty()) {
      const Box f = pad_box.inflated(kFallbackMargin);
      cpts[z(pt.side)] = {{f.x0, f.y0}, {f.x1, f.y0}, {f.x1, f.y1}, {f.x0, f.y1}};
    }
    std::array<Shape, 2> cy0;
    for (int s = 0; s < 2; ++s) {
      if (cpts[z(s)].size() < 3) continue;
      auto hull = convex_hull(cpts[z(s)]);
      if (hull.size() < 3) continue;
      for (auto& q : hull) q = q - fp.pos;
      cy0[z(s)] = Shape::polygon(std::move(hull), 0);
    }
    if (cy0[0].pts.empty() && cy0[1].pts.empty()) continue;  // nothing to place or avoid (logos, net ties without pads)

    for (int r = 0; r < 4; ++r) {
      PartGeom& g = pt.geom[z(r)];
      auto rot_shape = [&](const Shape& s) {
        Shape t = s;
        for (auto& q : t.pts) q = rot90(q, r);
        t.update_box();
        return t;
      };
      for (int s = 0; s < 2; ++s)
        if (!cy0[z(s)].pts.empty()) {
          g.cy[z(s)] = rot_shape(cy0[z(s)]);
          g.body.add(g.cy[z(s)].box);
          g.edge_box.add(g.cy[z(s)].box);
        }
      for (const auto& s : through) {
        g.through.push_back(rot_shape(s));
        g.body.add(g.through.back().box);
      }
      for (const auto& s : pads) {
        g.pads.push_back(rot_shape(s));
        g.edge_box.add(g.pads.back().box.inflated(p.edge_clearance));
      }
    }
    const Box bb = pt.geom[0].body;
    const long double area = static_cast<long double>(bb.x1 - bb.x0 + cc) * static_cast<long double>(bb.y1 - bb.y0 + cc);
    pt.area = static_cast<Coord>(std::min<long double>(area, 4e18L));
    pt.shape_key = std::hash<std::string>{}(fp.lib_id) * 31u + static_cast<std::uint64_t>(pt.side);

    // Movability.
    if (fp.locked) pt.fixed_reason = "locked";
    else if (fp.board_only) pt.fixed_reason = "board only";
    else if (fp.pads.empty()) pt.fixed_reason = "no pads";
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
        if (pt.geom[0].cy[z(s)].pts.empty()) continue;
        const Shape cy = translated(pt.geom[0].cy[z(s)], fp.pos);
        for (const auto& e : p.edges)
          if (geom::closer_than(cy, e, kEdgeConnectorReach)) {
            pt.fixed_reason = "edge connector";
            break;
          }
      }
    }
    pt.movable = pt.fixed_reason.empty();
    part_of_fp[fi] = static_cast<int>(p.parts.size());
    p.parts.push_back(std::move(pt));
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
  return p;
}

}  // namespace tmk::place
