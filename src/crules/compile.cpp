// Compiling effective rules into engine inputs (doc 15 §5.5): placement pseudo-nets, keep-out rule areas and the
// sidecar .kicad_dru.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "crules/engine.hpp"
#include "crules/names.hpp"
#include "geom/shape.hpp"

namespace tmk::crules {

namespace {

using model::NetId;

// Claim priority of a proximity rule's parts: lower claims first (doc 15 §3.4: a part is pulled by one rule).
int claim_priority(const std::string& id) {
  if (id.starts_with("XTAL-")) return 0;
  if (id.starts_with("OSC-")) return 1;
  if (id == "ESD-01" || id == "USB2-07" || id == "USBC-10" || id == "CAN-02" || id == "RS485-02") return 2;
  if (id.starts_with("LDO-") || id.starts_with("BUCK-") || id.starts_with("BOOST-")) return 3;
  return 4;  // DEC-*
}

const std::string& fp_ref(const model::Board& b, int pad) {
  return b.footprints[static_cast<std::size_t>(b.pads[static_cast<std::size_t>(pad)].footprint)].reference;
}

// Courtyard outline points of a footprint (absolute), else its pad corners.
std::vector<Point> outline_points(const model::Board& b, int fi) {
  std::vector<Point> pts;
  const auto& fp = b.footprints[static_cast<std::size_t>(fi)];
  for (int gi : fp.graphics) {
    const auto& g = b.graphics[static_cast<std::size_t>(gi)];
    if (g.layer != "F.CrtYd" && g.layer != "B.CrtYd") continue;
    switch (g.kind) {
      case model::Graphic::Kind::Line: pts.push_back(g.a); pts.push_back(g.b); break;
      case model::Graphic::Kind::Rect: pts.insert(pts.end(), {g.a, Point{g.b.x, g.a.y}, g.b, Point{g.a.x, g.b.y}}); break;
      case model::Graphic::Kind::Arc: {
        const auto a = geom::arc_points(g.a, g.c, g.b);
        pts.insert(pts.end(), a.begin(), a.end());
        break;
      }
      case model::Graphic::Kind::Circle: {
        const Coord r = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y)));
        const auto c = geom::circle_points(g.a, r);
        pts.insert(pts.end(), c.begin(), c.end());
        break;
      }
      default: pts.insert(pts.end(), g.pts.begin(), g.pts.end()); break;
    }
  }
  if (pts.empty())
    for (int pi : fp.pads) {
      const auto& p = b.pads[static_cast<std::size_t>(pi)];
      const Coord h = std::max(p.size_x, p.size_y) / 2;
      pts.insert(pts.end(), {Point{p.pos.x - h, p.pos.y - h}, Point{p.pos.x + h, p.pos.y - h}, Point{p.pos.x + h, p.pos.y + h}, Point{p.pos.x - h, p.pos.y + h}});
    }
  return pts;
}

// Convex hull grown by `margin` (each vertex replaced by a 16-gon of circumradius margin / cos(pi/16), so the
// result contains the exact offset polygon).
std::vector<Point> grown_hull(std::vector<Point> pts, Coord margin) {
  auto hull = geom::convex_hull(std::move(pts));
  if (margin <= 0 || hull.empty()) return hull;
  std::vector<Point> ring;
  const double rr = static_cast<double>(margin) / std::cos(std::numbers::pi / 16.0);
  for (const Point& q : hull)
    for (int k = 0; k < 16; ++k) {
      const double a = 2.0 * std::numbers::pi * k / 16.0;
      ring.push_back(Point{q.x + geom::kiround(rr * std::cos(a)), q.y + geom::kiround(rr * std::sin(a))});
    }
  return geom::convex_hull(std::move(ring));
}

struct KeepoutPlan {
  int instance = -1;
  const RuleSpec* spec = nullptr;
  std::vector<int> parts;   // footprints whose courtyards form the area
  std::vector<int> pins;    // extra pads whose centres are included (a crystal's IC oscillator pins)
  std::vector<NetId> own;   // nets allowed inside in the sidecar rule
};

std::vector<KeepoutPlan> keepout_plans(const model::Board& b, const Catalogue& cat, const Detection& det) {
  std::vector<KeepoutPlan> out;
  const BoardIndex ix(b);
  for (std::size_t ii = 0; ii < det.instances.size(); ++ii) {
    const Instance& in = det.instances[ii];
    if (in.confidence < cat.apply) continue;
    const Category& C = cat.categories[static_cast<std::size_t>(in.category)];
    for (const RuleSpec& r : C.rules) {
      if (in.disabled(r.id)) continue;  // user override file
      KeepoutPlan k;
      k.instance = static_cast<int>(ii);
      k.spec = &r;
      if (r.id == "XTAL-04") {
        k.parts.push_back(in.anchor);
        if (const Role* c = in.role("load_caps")) k.parts.insert(k.parts.end(), c->parts.begin(), c->parts.end());
        for (const char* nm : {"xin", "xout"})
          if (const Role* p = in.role(nm)) k.pins.insert(k.pins.end(), p->pads.begin(), p->pads.end());
      } else if (r.id == "BUCK-06" || r.id == "BOOST-04") {
        if (const Role* l = in.role("inductor")) k.parts = l->parts;
      } else {
        continue;
      }
      if (k.parts.empty()) continue;
      for (int f : k.parts)
        for (NetId n : ix.fp_nets(f)) k.own.push_back(n);
      std::sort(k.own.begin(), k.own.end());
      k.own.erase(std::unique(k.own.begin(), k.own.end()), k.own.end());
      out.push_back(std::move(k));
    }
  }
  return out;
}

}  // namespace

std::vector<PlacementAffinity> placement_affinities(const model::Board& b, const Catalogue& cat, const Detection& det, Mode mode) {
  std::vector<PlacementAffinity> out;
  if (mode != Mode::Soft && mode != Mode::On) return out;
  auto prox = proximity_pairs(b, cat, det);
  std::stable_sort(prox.begin(), prox.end(), [](const ProximityPairs& x, const ProximityPairs& y) {
    const int px = claim_priority(x.spec->id), py = claim_priority(y.spec->id);
    return px != py ? px < py : x.instance < y.instance;
  });
  std::map<int, std::pair<int, int>> claimed;  // satellite footprint -> (priority, instance) that owns it
  std::set<std::pair<int, int>> seen;
  for (const auto& pp : prox) {
    if (!pp.unbound.empty() || !pp.spec->enforced_in("place") || pp.spec->severity == Severity::Advisory) continue;
    const auto owner = std::make_pair(claim_priority(pp.spec->id), pp.instance);
    for (const auto& [pa, pb] : pp.pairs) {
      const int sat = b.pads[static_cast<std::size_t>(pa)].footprint;
      if (b.footprints[static_cast<std::size_t>(sat)].locked) continue;  // never pull a locked part (rule 6)
      const auto [it, fresh] = claimed.emplace(sat, owner);
      if (!fresh && it->second != owner) continue;
      if (!seen.emplace(pa, pb).second) continue;
      PlacementAffinity a;
      a.pad_a = pa;
      a.pad_b = pb;
      a.weight = pp.weight;
      a.rule = pp.spec->id;
      a.name = "~" + pp.spec->id + " " + fp_ref(b, pa) + "-" + fp_ref(b, pb);
      out.push_back(std::move(a));
    }
  }
  return out;
}

std::vector<GeneratedKeepout> generate_keepouts(const model::Board& b, const Catalogue& cat, const Detection& det, std::vector<std::string>* not_generated) {
  std::vector<GeneratedKeepout> out;
  const int nl = b.copper_count();
  for (const auto& k : keepout_plans(b, cat, det)) {
    const std::string& ref = b.footprints[static_cast<std::size_t>(det.instances[static_cast<std::size_t>(k.instance)].anchor)].reference;
    std::vector<Point> pts;
    model::LayerMask used = 0;
    for (int f : k.parts) {
      const auto o = outline_points(b, f);
      pts.insert(pts.end(), o.begin(), o.end());
      for (int pi : b.footprints[static_cast<std::size_t>(f)].pads) used |= b.pads[static_cast<std::size_t>(pi)].copper;
    }
    for (int p : k.pins) pts.push_back(b.pads[static_cast<std::size_t>(p)].pos);
    double margin_mm = 0;
    const auto& params = rule_params(det.instances[static_cast<std::size_t>(k.instance)], *k.spec);
    if (params.contains("margin_mm") && params["margin_mm"].is_number()) margin_mm = params["margin_mm"].get<double>();
    const auto poly = grown_hull(std::move(pts), mm_to_nm(margin_mm));
    if (poly.size() < 3) continue;
    // Layers where none of the parts has a pad (their own nets need no track there) and no other part has a pad
    // inside the area (its connections must stay possible).
    model::LayerMask free = 0;
    for (int l = 0; l < nl; ++l) {
      if (used & model::layer_bit(l)) continue;
      bool foreign = false;
      for (const auto& p : b.pads) {
        if (!(p.copper & model::layer_bit(l)) || std::find(k.parts.begin(), k.parts.end(), p.footprint) != k.parts.end()) continue;
        if (geom::point_in_polygon(p.pos, poly)) {
          foreign = true;
          break;
        }
      }
      if (!foreign) free |= model::layer_bit(l);
    }
    if (!free) {
      if (not_generated)
        not_generated->push_back(k.spec->id + " " + ref + ": no copper layer is free of the parts' pads (or other parts' pads inside the area)");
      continue;
    }
    GeneratedKeepout g;
    g.rule = k.spec->id;
    g.ref = ref;
    g.instance = k.instance;
    g.zone.rule_area = true;
    g.zone.keepout_tracks = true;
    g.zone.name = "tmk:" + k.spec->id + ":" + ref;
    g.zone.copper = free;
    for (int l = 0; l < nl; ++l)
      if (free & model::layer_bit(l)) g.zone.layers.push_back(b.copper_name(l));
    g.zone.outline.push_back(poly);
    out.push_back(std::move(g));
  }
  return out;
}

std::vector<std::pair<NetId, NetId>> usb_pairs(const model::Board& b, const Catalogue& cat, const Detection& det) {
  (void)b;
  std::vector<std::pair<NetId, NetId>> out;
  const int usb = cat.index_of("usb2");
  for (const auto& in : det.instances) {
    if (in.category != usb || in.disabled("USB2-02")) continue;
    const Role* p = in.role("dp");
    const Role* m = in.role("dm");
    if (!p || !m || p->nets.size() != 1 || m->nets.size() != 1 || p->nets.front() == m->nets.front()) continue;
    const std::pair<NetId, NetId> pr{p->nets.front(), m->nets.front()};
    if (std::find(out.begin(), out.end(), pr) == out.end()) out.push_back(pr);
  }
  return out;
}

std::string dru_sidecar(const model::Board& b, const Catalogue& cat, const Detection& det) {
  std::ostringstream s;
  s << "(version 1)\n";
  s << "# Generated by TraceMaker from component rules (docs/15-component-rules.md, catalogue v" << cat.version << ").\n";
  s << "# Review before merging into the project's own .kicad_dru; TraceMaker never edits that file.\n";
  int n = 0;
  for (const auto& k : keepout_plans(b, cat, det)) {
    const Instance& in = det.instances[static_cast<std::size_t>(k.instance)];
    const std::string& ref = b.footprints[static_cast<std::size_t>(in.anchor)].reference;
    std::string cond = "(";
    bool bad = false;
    for (std::size_t i = 0; i < k.parts.size(); ++i) {
      const std::string& r = b.footprints[static_cast<std::size_t>(k.parts[i])].reference;
      bad = bad || r.find('\'') != std::string::npos;
      cond += (i ? " || " : "") + std::string("A.intersectsCourtyard('") + r + "')";
    }
    cond += ")";
    // Allowed inside: the parts' own nets and every ground net (ground copper under a crystal is wanted).
    std::set<std::string> allowed;
    for (NetId net : k.own) allowed.insert(b.nets[static_cast<std::size_t>(net)].name);
    for (const auto& nt : b.nets)
      if (!nt.name.empty() && ground_like_name(nt.name)) allowed.insert(nt.name);
    for (const auto& a : allowed) {
      bad = bad || a.find('\'') != std::string::npos || a.find('"') != std::string::npos;
      cond += " && A.NetName != '" + a + "'";
    }
    if (bad) continue;  // a quote in a name would need escaping KiCad's expression syntax does not offer
    s << "\n# " << k.spec->id << " (" << severity_name(k.spec->severity) << ", confidence " << in.confidence << "): " << k.spec->text << "\n";
    s << "(rule \"tmk " << k.spec->id << " " << ref << "\"\n";
    s << "  (constraint disallow track via)\n";
    s << "  (condition \"" << cond << "\"))\n";
    ++n;
  }
  if (n == 0) s << "# (no generated rules for this board)\n";
  return s.str();
}

}  // namespace tmk::crules
