#include "drc/connectivity.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace tmk::drc {

namespace {
bool shapes_closer(const CopperItem& a, const CopperItem& c, Coord t) {
  for (const auto& s : a.shapes)
    for (const auto& u : c.shapes)
      if (geom::closer_than(s, u, t)) return true;
  return false;
}
}  // namespace

// Anchor-based connectivity, like KiCad's: tracks connect at their end points (end disks), vias and pads by
// their shapes (a pad links to any copper it overlaps), zone fills to anything they overlap, and net-tie pads
// to each other.
Connectivity compute_connectivity(const model::Board& b, const CopperModel& cm, index::UniformGrid& grid, bool overlap_links) {
  Connectivity out;
    const auto n = cm.items.size();
    UnionFind uf(n);
    auto& end_hit = out.end_hit;
    auto& via_layers = out.via_layers;
    end_hit.assign(n * 2, 0);
    via_layers.assign(n, 0);
    auto anchor_r = [&](const CopperItem& it) -> Coord {
      return (it.kind == ItemKind::Track || it.kind == ItemKind::Arc || it.kind == ItemKind::Via) ? it.width / 2 : 0;
    };
    auto anchors = [&](const CopperItem& it, std::vector<model::Point>& pts_out) {
      pts_out.clear();
      if (it.kind == ItemKind::Track) {
        const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
        pts_out = {t.a, t.b};
      } else if (it.kind == ItemKind::Arc) {
        const auto& t = b.arcs[static_cast<std::size_t>(it.index)];
        pts_out = {t.a, t.b};
      } else if (it.kind == ItemKind::Via || it.kind == ItemKind::Pad) {
        pts_out = {it.pos};
      }
    };
    std::vector<model::Point> anc;
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& a = cm.items[i];
      if (a.net == 0) continue;
      grid.query(a.box, [&](int jj) {
        const auto j = static_cast<std::size_t>(jj);
        if (j <= i) return;
        const CopperItem& c = cm.items[j];
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
          // Two pads need a centre inside the other (checked by the anchor pass above): overlapping pads of
          // different footprints do not connect (verified on PCBench Horticulture's via-stitching arrays).
          if (!linked && (a.kind == ItemKind::Pad) != (c.kind == ItemKind::Pad)) linked = shapes_closer(a, c, 1);
          // KiCad 10 (CN_VISITOR) links any two connectivity items whose copper collides, not only at end points.
          if (!linked && overlap_links && a.kind != ItemKind::Pad && c.kind != ItemKind::Pad && in_item_graph(a) && in_item_graph(c))
            linked = shapes_closer(a, c, 0);  // strict, like SHAPE::Collide
          // Overlapping pads of the same footprint do connect (KiCad: RoyalBlue54L J2, three overlapping pads "1").
          if (!linked && a.kind == ItemKind::Pad && c.kind == ItemKind::Pad && a.footprint >= 0 && a.footprint == c.footprint)
            linked = shapes_closer(a, c, 1);
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
        if (cm.items[i].kind == ItemKind::Pad) {
          const auto& p = b.pads[static_cast<std::size_t>(cm.items[i].index)];
          pad_item[{p.footprint, p.number}] = static_cast<int>(i);
        }
      for (std::size_t f = 0; f < b.footprints.size(); ++f)
        for (const auto& g : b.footprints[f].net_tie_groups)
          for (std::size_t k = 1; k < g.size(); ++k) {
            const auto x = pad_item.find({static_cast<int>(f), g[0]}), y = pad_item.find({static_cast<int>(f), g[k]});
            if (x != pad_item.end() && y != pad_item.end()) uf.unite(x->second, y->second);
          }
    }
    out.root.resize(n);
    for (std::size_t i = 0; i < n; ++i) out.root[i] = uf.find(static_cast<int>(i));
    return out;
}

bool in_item_graph(const CopperItem& it) {
  return it.kind != ItemKind::Graphic || it.footprint < 0;  // footprint graphics are not connectivity items
}

bool can_change_net(const CopperItem& it) {
  return it.kind == ItemKind::Track || it.kind == ItemKind::Arc || (it.kind == ItemKind::Via && !it.free_via) ||
         it.kind == ItemKind::Graphic;
}

ItemGraph item_graph(const model::Board& /*b*/, const CopperModel& cm, index::UniformGrid& grid) {
  ItemGraph g;
  const auto n = cm.items.size();
  g.adj.assign(n, {});
  for (std::size_t i = 0; i < n; ++i) {
    const CopperItem& a = cm.items[i];
    if (!in_item_graph(a)) continue;
    grid.query(a.box, [&](int jj) {
      const auto j = static_cast<std::size_t>(jj);
      if (j <= i) return;
      const CopperItem& c = cm.items[j];
      if (!in_item_graph(c) || !(a.layers & c.layers) || a.kind == ItemKind::Zone || c.kind == ItemKind::Zone) return;
      const bool fixed = !can_change_net(a) && !can_change_net(c);
      if (fixed && a.net != c.net) return;  // CN_VISITOR: "don't connect items in different nets that can't be changed"
      bool linked = false;
      if (a.kind == ItemKind::Pad && c.kind == ItemKind::Pad) {
        // Same rule as compute_connectivity: a centre inside the other pad, or overlapping pads of one footprint.
        for (int pass = 0; pass < 2 && !linked; ++pass) {
          const CopperItem& x = pass == 0 ? a : c;
          const CopperItem& y = pass == 0 ? c : a;
          for (const auto& s : y.shapes)
            if (geom::closer_than(geom::Shape::point(x.pos, 0), s, 1)) { linked = true; break; }
        }
        if (!linked && a.footprint >= 0 && a.footprint == c.footprint) linked = shapes_closer(a, c, 1);
      } else {
        linked = shapes_closer(a, c, 0);  // strict overlap: KiCad's SHAPE::Collide does not link touching copper
      }
      if (linked) {
        g.adj[i].push_back(static_cast<int>(j));
        g.adj[j].push_back(static_cast<int>(i));
      }
    });
  }
  for (auto& v : g.adj) std::sort(v.begin(), v.end());
  return g;
}

ZoneFills::ZoneFills(const CopperModel& cm) : slot(cm.items.size(), -1) {
  for (std::size_t i = 0; i < cm.items.size(); ++i) {
    const CopperItem& z = cm.items[i];
    if (z.kind != ItemKind::Zone || z.shapes.size() != 1 || !z.shapes[0].closed || z.shapes[0].r != 0) continue;
    slot[i] = static_cast<int>(index.size());
    index.emplace_back(z.shapes[0].pts);
  }
}

std::vector<int> zones_touching(const CopperModel& cm, const ZoneFills& zf, index::UniformGrid& grid, const geom::Shape& s,
                                model::LayerMask layers, Coord clearance) {
  std::vector<int> out;
  const bool disk = s.pts.size() == 1 && !s.closed;
  grid.query(s.box, [&](int j) {
    const CopperItem& z = cm.items[static_cast<std::size_t>(j)];
    if (z.kind != ItemKind::Zone || !(z.layers & layers) || !z.box.intersects(s.box)) return;
    const int k = zf.slot[static_cast<std::size_t>(j)];
    if (disk && k >= 0) {
      if (zf.index[static_cast<std::size_t>(k)].disk_closer(s.pts[0], s.r, clearance)) out.push_back(j);
      return;
    }
    for (const auto& u : z.shapes)
      if (geom::closer_than(s, u, clearance)) {
        out.push_back(j);
        return;
      }
  });
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<model::NetId> propagate_nets(const CopperModel& cm, const ItemGraph& g, const ZoneFills& zf, index::UniformGrid& grid) {
  const auto n = cm.items.size();
  std::vector<model::NetId> net(n);
  for (std::size_t i = 0; i < n; ++i) net[i] = cm.items[i].net;
  // Vias that touch only zone fills take a zone's net (CN_CONNECTIVITY_ALGO::searchConnections, deferred net codes).
  for (std::size_t i = 0; i < n; ++i) {
    const CopperItem& v = cm.items[i];
    if (v.kind != ItemKind::Via || v.free_via || !g.adj[i].empty()) continue;  // linked to a track or pad: clusters decide
    std::set<model::NetId> zone_nets;
    for (const auto& s : v.shapes)
      for (const int j : zones_touching(cm, zf, grid, s, v.layers)) zone_nets.insert(cm.items[static_cast<std::size_t>(j)].net);
    if (!zone_nets.empty() && !zone_nets.count(v.net)) net[i] = *zone_nets.begin();
  }
  // Clusters without zones (SearchClusters(CSM_PROPAGATE)); origin net and conflicts as in CN_CLUSTER::Add.
  std::vector<char> seen(n, 0);
  std::vector<int> members;
  std::deque<int> q;
  for (std::size_t r = 0; r < n; ++r) {
    if (seen[r] || !in_item_graph(cm.items[r]) || cm.items[r].kind == ItemKind::Zone) continue;
    members.clear();
    seen[r] = 1;
    q.push_back(static_cast<int>(r));
    model::NetId origin = 0;
    bool origin_pad = false, conflicting = false;
    std::map<model::NetId, int> rank;
    while (!q.empty()) {
      const int cur = q.front();
      q.pop_front();
      members.push_back(cur);
      const auto cu = static_cast<std::size_t>(cur);
      const model::NetId nc = net[cu];
      if (nc > 0) {
        if (origin <= 0) {
          origin = nc;
          rank[origin] = 0;
        }
        if (cm.items[cu].kind == ItemKind::Pad) {
          const int rk = ++rank[nc];
          if (!origin_pad || rk > rank[origin]) {
            origin_pad = true;
            origin = nc;
          }
          if (nc != origin) conflicting = true;
        }
      }
      for (const int j : g.adj[cu]) {
        const auto ju = static_cast<std::size_t>(j);
        if (seen[ju] || cm.items[ju].kind == ItemKind::Zone) continue;
        seen[ju] = 1;
        q.push_back(j);
      }
    }
    if (conflicting || origin <= 0) continue;
    for (const int m : members)
      if (can_change_net(cm.items[static_cast<std::size_t>(m)])) net[static_cast<std::size_t>(m)] = origin;
  }
  return net;
}

}  // namespace tmk::drc
