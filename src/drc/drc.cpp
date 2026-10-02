#include "drc/drc.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <nlohmann/json.hpp>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "index/uniform_grid.hpp"

namespace tmk::drc {

std::map<std::string, int> DrcReport::counts() const {
  std::map<std::string, int> c;
  for (const auto& v : violations) ++c[v.type];
  if (!unconnected.empty()) c["unconnected_items"] = static_cast<int>(unconnected.size());
  return c;
}

namespace {

struct UnionFind {
  std::vector<int> p;
  explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) {
    while (p[static_cast<std::size_t>(x)] != x) x = p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
    return x;
  }
  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) p[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
  }
};

class Checker {
 public:
  Checker(const model::Board& b, const model::DesignRules& r, const DrcOptions& o)
      : b_(b), r_(r), o_(o), cm_(build_copper(b)), re_(b, r, cm_) {}

  DrcReport run() {
    rep_.warnings = r_.warnings;
    for (const auto& w : re_.warnings()) rep_.warnings.push_back(w);
    build_grid();
    check_pairs();
    check_items();
    check_pad_rings();
    check_holes();
    check_edges();
    check_keepouts();
    check_connectivity();
    // Project severities: drop ignored types, apply warning/error levels.
    auto sev = [&](const std::string& t) -> const std::string* {
      const auto it = r_.severities.find(t);
      return it == r_.severities.end() ? nullptr : &it->second;
    };
    std::erase_if(rep_.violations, [&](const Violation& v) { const auto* s = sev(v.type); return s && *s == "ignore"; });
    for (auto& v : rep_.violations)
      if (const auto* s = sev(v.type)) v.severity = *s;
    if (const auto* s = sev("unconnected_items"); s && *s == "ignore") rep_.unconnected.clear();
    std::stable_sort(rep_.violations.begin(), rep_.violations.end(), [](const Violation& a, const Violation& c) { return a.type < c.type; });
    return std::move(rep_);
  }

 private:
  std::string describe(const CopperItem& it) const {
    const std::string net = it.net ? " [" + b_.nets[static_cast<std::size_t>(it.net)].name + "]" : "";
    switch (it.kind) {
      case ItemKind::Pad: {
        const auto& p = b_.pads[static_cast<std::size_t>(it.index)];
        return "Pad " + p.number + " of " + b_.footprints[static_cast<std::size_t>(p.footprint)].reference + net;
      }
      case ItemKind::Track: return "Track" + net + " on " + b_.copper_name(b_.tracks[static_cast<std::size_t>(it.index)].layer);
      case ItemKind::Arc: return "Arc" + net;
      case ItemKind::Via: return "Via" + net;
      case ItemKind::Zone: return "Zone" + net;
      case ItemKind::Graphic: return "Copper graphic";
    }
    return "?";
  }
  void add(std::string type, const CopperItem* a, const CopperItem* c, Coord actual, Coord required, int layer, std::string sev = "error") {
    Violation v;
    v.type = std::move(type);
    v.severity = std::move(sev);
    v.actual = actual;
    v.required = required;
    v.layer = layer;
    if (a) v.items.push_back({describe(*a), a->pos});
    if (c) v.items.push_back({describe(*c), c->pos});
    rep_.violations.push_back(std::move(v));
  }

  void build_grid() {
    geom::Box all;
    for (const auto& it : cm_.items) all.add(it.box);
    for (const auto& e : cm_.edges) all.add(e.box);
    if (all.empty()) all = geom::Box{0, 0, 1, 1};
    reach_ = re_.max_clearance() + 1;
    all = all.inflated(reach_);
    const Coord cell = std::max<Coord>(1'000'000, 2 * reach_);
    grid_ = std::make_unique<index::UniformGrid>(all, cell, cm_.items.size());
    for (std::size_t i = 0; i < cm_.items.size(); ++i) grid_->insert(static_cast<int>(i), cm_.items[i].box);
  }

  static bool shapes_closer(const CopperItem& a, const CopperItem& c, Coord t) {
    for (const auto& s : a.shapes)
      for (const auto& u : c.shapes)
        if (geom::closer_than(s, u, t)) return true;
    return false;
  }
  static double shapes_gap(const CopperItem& a, const CopperItem& c) {
    double g = 1e30;
    for (const auto& s : a.shapes)
      for (const auto& u : c.shapes) g = std::min(g, geom::gap(s, u));
    return g;
  }

  void check_pairs() {
    const auto n = cm_.items.size();
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& a = cm_.items[i];
      grid_->query(a.box.inflated(reach_), [&](int j) {
        if (static_cast<std::size_t>(j) <= i) return;
        const CopperItem& c = cm_.items[static_cast<std::size_t>(j)];
        const model::LayerMask common = a.layers & c.layers;
        if (!common) return;
        if (a.net == c.net && a.net != 0) return;
        if (a.kind == ItemKind::Zone && c.kind == ItemKind::Zone) return;
        if (a.footprint >= 0 && a.footprint == c.footprint) {
          // A footprint's own copper graphics are not checked against its pads when those carry a net, or in
          // net-tie footprints (KiCad: One-Air-Max USB1 shield copper); net-less pads are (microwave POLY).
          if ((a.kind == ItemKind::Graphic || c.kind == ItemKind::Graphic) &&
              (a.net != 0 || c.net != 0 || !b_.footprints[static_cast<std::size_t>(a.footprint)].net_tie_groups.empty())) return;
        }
        const int layer = std::countr_zero(common);
        // Track centre lines crossing.
        if (a.kind == ItemKind::Track && c.kind == ItemKind::Track && a.net && c.net) {
          const auto& ta = b_.tracks[static_cast<std::size_t>(a.index)];
          const auto& tc = b_.tracks[static_cast<std::size_t>(c.index)];
          if (geom::segments_intersect(ta.a, ta.b, tc.a, tc.b)) {
            add("tracks_crossing", &a, &c, 0, 0, layer);
            return;
          }
        }
        const Coord req = re_.clearance(a, c, layer);
        if (!shapes_closer(a, c, req - o_.epsilon)) return;
        const double g = shapes_gap(a, c);
        // KiCad: overlapping copper of two different nets, or of two net-less items, is a short; when only one
        // side has a net it is a clearance violation with zero gap.
        if (g <= 0 && ((a.net && c.net) || (!a.net && !c.net))) add("shorting_items", &a, &c, 0, req, layer);
        else add("clearance", &a, &c, static_cast<Coord>(std::max(0.0, g)), req, layer);
      });
    }
  }

  void check_items() {
    for (const auto& it : cm_.items) {
      if (it.kind == ItemKind::Track || it.kind == ItemKind::Arc) {
        const int layer = std::countr_zero(it.layers);
        const auto [mn, mx] = re_.track_width(it, layer);
        if (it.width < mn || (mx > 0 && it.width > mx)) add("track_width", &it, nullptr, it.width, mn, layer);
      } else if (it.kind == ItemKind::Via) {
        const auto& v = b_.vias[static_cast<std::size_t>(it.index)];
        if (const Coord mn = re_.via_diameter_min(it); v.size < mn) add("via_diameter", &it, nullptr, v.size, mn, -1);
        if (const Coord mn = re_.annular_width_min(it); (v.size - v.drill) / 2 < mn) add("annular_width", &it, nullptr, (v.size - v.drill) / 2, mn, -1);
      }
    }
  }

  void check_pad_rings() {
    for (const auto& it : cm_.items) {
      if (it.kind != ItemKind::Pad) continue;
      const auto& p = b_.pads[static_cast<std::size_t>(it.index)];
      if (p.type != model::PadType::ThruHole || p.drill_x <= 0) continue;
      const Coord ring = std::min((p.size_x - p.drill_x) / 2, (p.size_y - (p.drill_oval ? p.drill_y : p.drill_x)) / 2);
      const Coord mn = re_.annular_width_min(it);
      if (ring < mn) add("annular_width", &it, nullptr, std::max<Coord>(ring, 0), mn, -1);
    }
  }

  void check_holes() {
    const auto& holes = cm_.holes;
    // Hole size.
    for (const auto& h : holes) {
      const CopperItem* owner = h.item >= 0 ? &cm_.items[static_cast<std::size_t>(h.item)] : nullptr;
      const Coord d = h.shape.r * 2;
      if (owner && owner->kind == ItemKind::Via && d < re_.hole_size_min(owner)) add("drill_out_of_range", owner, nullptr, d, re_.hole_size_min(owner), -1);
    }
    // Hole to hole and hole to copper.
    geom::Box all;
    for (const auto& h : holes) all.add(h.shape.box);
    if (all.empty()) return;
    index::UniformGrid hg(all.inflated(reach_), std::max<Coord>(1'000'000, 2 * reach_), holes.size());
    for (std::size_t i = 0; i < holes.size(); ++i) hg.insert(static_cast<int>(i), holes[i].shape.box);
    for (std::size_t i = 0; i < holes.size(); ++i) {
      const Hole& a = holes[i];
      const CopperItem* ao = a.item >= 0 ? &cm_.items[static_cast<std::size_t>(a.item)] : nullptr;
      hg.query(a.shape.box.inflated(reach_), [&](int j) {
        if (static_cast<std::size_t>(j) <= i) return;
        const Hole& c = holes[static_cast<std::size_t>(j)];
        const CopperItem* co = c.item >= 0 ? &cm_.items[static_cast<std::size_t>(c.item)] : nullptr;
        if (a.pad >= 0 && c.pad >= 0) {
          const auto& pa = b_.pads[static_cast<std::size_t>(a.pad)];
          const auto& pc = b_.pads[static_cast<std::size_t>(c.pad)];
          if (a.pad == c.pad || (pa.footprint == pc.footprint && pa.number == pc.number && !pa.number.empty())) return;
        }
        if (a.shape.pts.size() == 1 && c.shape.pts.size() == 1 && a.shape.pts[0] == c.shape.pts[0]) {
          Violation v;
          v.type = "holes_co_located";
          v.severity = "warning";
          v.items.push_back({ao ? describe(*ao) : "Hole", a.pos});
          v.items.push_back({co ? describe(*co) : "Hole", c.pos});
          rep_.violations.push_back(std::move(v));
          return;
        }
        const Coord req = re_.hole_to_hole(ao, co);
        if (req > 0 && geom::closer_than(a.shape, c.shape, req - o_.epsilon)) {
          Violation v;
          v.type = "hole_to_hole";
          v.items.push_back({ao ? describe(*ao) : "Hole", a.pos});
          v.items.push_back({co ? describe(*co) : "Hole", c.pos});
          v.required = req;
          rep_.violations.push_back(std::move(v));
        }
      });
      // Hole to copper of other nets.
      grid_->query(a.shape.box.inflated(reach_), [&](int j) {
        const CopperItem& c = cm_.items[static_cast<std::size_t>(j)];
        if (j == a.item) return;
        if (c.net == a.net && a.net != 0) return;
        if (c.kind == ItemKind::Zone) return;  // zone fills are cut around holes by the filler
        if (a.pad >= 0 && c.footprint >= 0 && c.footprint == b_.pads[static_cast<std::size_t>(a.pad)].footprint) return;  // own footprint
        if (a.pad >= 0 && c.kind == ItemKind::Pad && b_.pads[static_cast<std::size_t>(c.index)].footprint == b_.pads[static_cast<std::size_t>(a.pad)].footprint && !a.plated) return;
        const int layer = std::countr_zero(c.layers);
        const Coord req = re_.hole_clearance(ao, c, layer);
        if (req <= 0) return;
        for (const auto& s : c.shapes)
          if (geom::closer_than(a.shape, s, req - o_.epsilon)) {
            add("hole_clearance", &c, ao, -1, req, layer);
            break;
          }
      });
    }
  }

  void check_edges() {
    if (cm_.edges.empty()) return;
    for (const auto& it : cm_.items) {
      const int layer = std::countr_zero(it.layers);
      const Coord req = re_.edge_clearance(it, layer);
      if (req <= 0) continue;
      for (const auto& e : cm_.edges) {
        if (!e.box.inflated(req).intersects(it.box)) continue;
        bool hit = false;
        for (const auto& s : it.shapes)
          if (geom::closer_than(s, e, req - o_.epsilon)) { hit = true; break; }
        if (hit) {
          add("copper_edge_clearance", &it, nullptr, -1, req, layer);
          break;
        }
      }
    }
  }

  void check_keepouts() {
    for (const auto& z : b_.zones) {
      if (!z.rule_area || z.outline.empty() || z.outline.front().size() < 3) continue;
      const geom::Shape area = geom::Shape::polygon(z.outline.front(), 0);
      for (const auto& it : cm_.items) {
        if (!(it.layers & z.copper)) continue;
        const bool forbidden = ((it.kind == ItemKind::Track || it.kind == ItemKind::Arc) && z.keepout_tracks) ||
                               (it.kind == ItemKind::Via && z.keepout_vias) || (it.kind == ItemKind::Pad && z.keepout_pads);
        if (!forbidden || !area.box.intersects(it.box)) continue;
        bool hit = false;
        for (const auto& s : it.shapes)
          if (geom::closer_than(s, area, 1)) { hit = true; break; }
        if (hit) add("items_not_allowed", &it, nullptr, -1, -1, std::countr_zero(it.layers & z.copper));
      }
    }
  }

  // Anchor-based connectivity, like KiCad's: tracks connect at their end points, vias and pads at their
  // shapes; zone fills connect to anything they overlap.
  void check_connectivity() {
    const auto n = cm_.items.size();
    UnionFind uf(n);
    std::vector<std::uint8_t> end_hit(n * 2, 0);  // track end point connected?
    std::vector<model::LayerMask> via_layers(n, 0);  // layers on which each via has a connection
    auto anchor_r = [&](const CopperItem& it) -> Coord {
      return (it.kind == ItemKind::Track || it.kind == ItemKind::Arc || it.kind == ItemKind::Via) ? it.width / 2 : 0;
    };
    auto anchors = [&](const CopperItem& it, std::vector<model::Point>& out) {
      out.clear();
      if (it.kind == ItemKind::Track) {
        const auto& t = b_.tracks[static_cast<std::size_t>(it.index)];
        out = {t.a, t.b};
      } else if (it.kind == ItemKind::Arc) {
        const auto& t = b_.arcs[static_cast<std::size_t>(it.index)];
        out = {t.a, t.b};
      } else if (it.kind == ItemKind::Via || it.kind == ItemKind::Pad) {
        out = {it.pos};
      }
    };
    std::vector<model::Point> anc;
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& a = cm_.items[i];
      if (a.net == 0) continue;
      grid_->query(a.box, [&](int jj) {
        const auto j = static_cast<std::size_t>(jj);
        if (j <= i) return;
        const CopperItem& c = cm_.items[j];
        if (c.net != a.net || !(a.layers & c.layers)) return;
        bool linked = false;
        if (a.kind == ItemKind::Zone || c.kind == ItemKind::Zone) {
          linked = shapes_closer(a, c, 1);
          // A track end inside the fill is a connected end.
          const bool a_zone = a.kind == ItemKind::Zone;
          const CopperItem& t = a_zone ? c : a;
          const CopperItem& z = a_zone ? a : c;
          const std::size_t ti = a_zone ? j : i;
          if (linked && (t.kind == ItemKind::Track || t.kind == ItemKind::Arc)) {
            anchors(t, anc);
            for (std::size_t k = 0; k < anc.size(); ++k)
              if (geom::point_in_polygon(anc[k], z.shapes.front().pts)) end_hit[ti * 2 + k] = 1;
          }
        } else {
          // Anchors of a inside c, or anchors of c inside a.
          for (int pass = 0; pass < 2; ++pass) {
            const CopperItem& x = pass == 0 ? a : c;
            const CopperItem& y = pass == 0 ? c : a;
            const std::size_t xi = pass == 0 ? i : j;
            anchors(x, anc);
            for (std::size_t k = 0; k < anc.size(); ++k) {
              const geom::Shape pt = geom::Shape::point(anc[k], anchor_r(x));
              bool in = false;
              for (const auto& s : y.shapes)
                if (geom::closer_than(pt, s, 1)) { in = true; break; }
              if (in) {
                linked = true;
                if (x.kind == ItemKind::Track || x.kind == ItemKind::Arc) end_hit[xi * 2 + k] = 1;
              }
            }
          }
          // KiCad links pads to anything their copper overlaps (a track passing through a pad connects to it).
          if (!linked && (a.kind == ItemKind::Pad || c.kind == ItemKind::Pad)) linked = shapes_closer(a, c, 1);
        }
        if (linked) {
          uf.unite(static_cast<int>(i), static_cast<int>(j));
          if (a.kind == ItemKind::Via) via_layers[i] |= a.layers & c.layers;
          if (c.kind == ItemKind::Via) via_layers[j] |= a.layers & c.layers;
          // Track ends that touch another track's end also count as connected for both.
          if ((a.kind == ItemKind::Track || a.kind == ItemKind::Arc) && (c.kind == ItemKind::Track || c.kind == ItemKind::Arc)) {
            anchors(a, anc);
            for (std::size_t k = 0; k < anc.size(); ++k)
              for (const auto& s : c.shapes)
                if (geom::closer_than(geom::Shape::point(anc[k], anchor_r(a)), s, 1)) end_hit[i * 2 + k] = 1;
            anchors(c, anc);
            for (std::size_t k = 0; k < anc.size(); ++k)
              for (const auto& s : a.shapes)
                if (geom::closer_than(geom::Shape::point(anc[k], anchor_r(c)), s, 1)) end_hit[j * 2 + k] = 1;
          }
        }
      });
    }
    // Pads joined by a net-tie footprint's copper are connected.
    {
      std::map<std::pair<int, std::string>, int> pad_item;
      for (std::size_t i = 0; i < n; ++i)
        if (cm_.items[i].kind == ItemKind::Pad) {
          const auto& p = b_.pads[static_cast<std::size_t>(cm_.items[i].index)];
          pad_item[{p.footprint, p.number}] = static_cast<int>(i);
        }
      for (std::size_t f = 0; f < b_.footprints.size(); ++f)
        for (const auto& g : b_.footprints[f].net_tie_groups)
          for (std::size_t k = 1; k < g.size(); ++k) {
            const auto x = pad_item.find({static_cast<int>(f), g[0]}), y = pad_item.find({static_cast<int>(f), g[k]});
            if (x != pad_item.end() && y != pad_item.end()) uf.unite(x->second, y->second);
          }
    }
    // Clusters with at least one pad, per net.
    std::map<model::NetId, std::vector<int>> roots;  // net -> distinct cluster roots containing pads
    std::map<int, int> root_item;
    // KiCad's ratsnest joins every copper cluster of a net (pads, and also stray tracks/vias), not only
    // clusters that contain pads (verified on fuzzed boards). Zone fills and graphics do not form clusters.
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& it = cm_.items[i];
      if (it.net == 0 || it.kind == ItemKind::Zone || it.kind == ItemKind::Graphic) continue;
      const int r = uf.find(static_cast<int>(i));
      auto& v = roots[it.net];
      if (std::find(v.begin(), v.end(), r) == v.end()) {
        v.push_back(r);
        root_item[r] = static_cast<int>(i);
      }
    }
    for (const auto& [net, rs] : roots)
      for (std::size_t k = 1; k < rs.size(); ++k) {
        Violation v;
        v.type = "unconnected_items";
        v.description = "Missing connection in net " + b_.nets[static_cast<std::size_t>(net)].name;
        v.items.push_back({describe(cm_.items[static_cast<std::size_t>(root_item[rs[k - 1]])]), cm_.items[static_cast<std::size_t>(root_item[rs[k - 1]])].pos});
        v.items.push_back({describe(cm_.items[static_cast<std::size_t>(root_item[rs[k]])]), cm_.items[static_cast<std::size_t>(root_item[rs[k]])].pos});
        rep_.unconnected.push_back(std::move(v));
      }
    if (!o_.dangling) return;
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& it = cm_.items[i];
      if (it.kind == ItemKind::Track || it.kind == ItemKind::Arc) {
        if (!end_hit[i * 2] || !end_hit[i * 2 + 1]) add("track_dangling", &it, nullptr, -1, -1, std::countr_zero(it.layers), "warning");
      } else if (it.kind == ItemKind::Via && std::popcount(via_layers[i]) < 2) {  // "connected on only one layer"
        add("via_dangling", &it, nullptr, -1, -1, -1, "warning");
      }
    }
  }

  const model::Board& b_;
  const model::DesignRules& r_;
  const DrcOptions& o_;
  CopperModel cm_;
  RuleEngine re_;
  std::unique_ptr<index::UniformGrid> grid_;
  Coord reach_ = 0;
  DrcReport rep_;

};

}  // namespace

DrcReport run_drc(const model::Board& b, const model::DesignRules& r, const DrcOptions& opt) { return Checker(b, r, opt).run(); }

void write_drc_json(const DrcReport& rep, const std::string& path) {
  using nlohmann::json;
  auto item_json = [](const ViolationItem& it) {
    return json{{"description", it.description}, {"pos", {{"x", nm_to_mm(it.pos.x)}, {"y", nm_to_mm(it.pos.y)}}}};
  };
  auto vjson = [&](const Violation& v) {
    json items = json::array();
    for (const auto& it : v.items) items.push_back(item_json(it));
    json j{{"type", v.type}, {"severity", v.severity}, {"description", v.description}, {"items", items}};
    if (v.required >= 0) j["required_mm"] = nm_to_mm(v.required);
    if (v.actual >= 0) j["actual_mm"] = nm_to_mm(v.actual);
    return j;
  };
  json d{{"source", "tracemaker"}, {"coordinate_units", "mm"}, {"violations", json::array()}, {"unconnected_items", json::array()}};
  for (const auto& v : rep.violations) d["violations"].push_back(vjson(v));
  for (const auto& v : rep.unconnected) d["unconnected_items"].push_back(vjson(v));
  d["warnings"] = rep.warnings;
  std::ofstream(path) << d.dump(1);
}

}  // namespace tmk::drc
