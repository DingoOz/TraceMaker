#include "drc/connectivity.hpp"

#include <map>

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
Connectivity compute_connectivity(const model::Board& b, const CopperModel& cm, index::UniformGrid& grid) {
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

}  // namespace tmk::drc
