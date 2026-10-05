// SPDX-License-Identifier: GPL-3.0-or-later
#include "model/board.hpp"

namespace tmk::model {

int Board::copper_index(std::string_view name) const {
  for (std::size_t i = 0; i < copper.size(); ++i) {
    const auto& l = layers[static_cast<std::size_t>(copper[i])];
    if (l.name == name || l.file_name == name) return static_cast<int>(i);
  }
  return -1;
}

NetId Board::net_by_name(std::string_view name) const {
  const auto it = net_index.find(std::string(name));
  return it == net_index.end() ? 0 : it->second;
}

Box Board::edge_bbox() const {
  Box b;
  for (const auto& g : graphics) {
    if (g.layer != "Edge.Cuts") continue;
    switch (g.kind) {
      case Graphic::Kind::Line: b.add(g.a); b.add(g.b); break;
      case Graphic::Kind::Arc: b.add(g.a); b.add(g.b); b.add(g.c); break;
      case Graphic::Kind::Circle: {
        const double dx = static_cast<double>(g.b.x - g.a.x), dy = static_cast<double>(g.b.y - g.a.y);
        const Coord r = geom::kiround(std::sqrt(dx * dx + dy * dy));
        b.add(Point{g.a.x - r, g.a.y - r});
        b.add(Point{g.a.x + r, g.a.y + r});
        break;
      }
      default: for (const auto& p : g.pts) b.add(p); break;
    }
  }
  return b;
}

}  // namespace tmk::model
