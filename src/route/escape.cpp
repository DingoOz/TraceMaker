#include "route/escape.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace tmk::route {

namespace {

std::size_t z(int i) { return static_cast<std::size_t>(i); }

// Half extents of a pad's bounding box along x and y (rotation applied).
std::pair<Coord, Coord> half_extents(const model::Pad& p) {
  const double a = p.angle * M_PI / 180.0;
  const double c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
  const double hx = c * static_cast<double>(p.size_x) / 2 + s * static_cast<double>(p.size_y) / 2;
  const double hy = s * static_cast<double>(p.size_x) / 2 + c * static_cast<double>(p.size_y) / 2;
  return {static_cast<Coord>(std::llround(hx)), static_cast<Coord>(std::llround(hy))};
}

int lowest_layer(model::LayerMask m) {
  for (int l = 0; l < 64; ++l)
    if (m & (model::LayerMask{1} << l)) return l;
  return -1;
}

}  // namespace

std::vector<EscapeCorridor> plan_escapes(const model::Board& b, const std::vector<char>& needs, const std::function<Coord(model::NetId)>& keep,
                                         const EscapeOptions& o, EscapeStats* stats) {
  std::vector<EscapeCorridor> out;
  for (const auto& fp : b.footprints) {
    std::vector<int> pads;
    for (int pi : fp.pads) {
      const auto& p = b.pads[z(pi)];
      if (p.copper != 0 && p.type != model::PadType::NpThruHole) pads.push_back(pi);
    }
    if (static_cast<int>(pads.size()) < o.min_pads) continue;
    // Pin pitch: the smallest centre distance between two pads of the footprint (pads at the same spot, e.g.
    // a thermal pad split into parts, do not count).
    Coord pitch = LLONG_MAX;
    for (std::size_t i = 0; i < pads.size(); ++i)
      for (std::size_t j = i + 1; j < pads.size(); ++j) {
        const Point d = b.pads[z(pads[j])].pos - b.pads[z(pads[i])].pos;
        const Coord dist = static_cast<Coord>(std::llround(std::hypot(static_cast<double>(d.x), static_cast<double>(d.y))));
        if (dist > 0) pitch = std::min(pitch, dist);
      }
    if (pitch == LLONG_MAX || pitch > o.max_pitch) continue;
    Coord x0 = LLONG_MAX, y0 = LLONG_MAX, x1 = LLONG_MIN, y1 = LLONG_MIN;
    for (int pi : pads) {
      const Point q = b.pads[z(pi)].pos;
      x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    const Coord ring = pitch * 3 / 4;  // within this of the pad-centre box: on the perimeter
    const Point centre{(x0 + x1) / 2, (y0 + y1) / 2};
    bool any = false;
    for (int pi : pads) {
      if (!needs[z(pi)]) continue;
      const auto& p = b.pads[z(pi)];
      if (p.type != model::PadType::Smd) continue;  // through-hole pins reach every layer already
      const int layer = lowest_layer(p.copper);
      if (layer < 0) continue;
      const Coord k = keep(p.net);
      const Point a = p.pos;
      const Coord dl = a.x - x0, dr = x1 - a.x, dt = a.y - y0, db = y1 - a.y;
      const Coord m = std::min({dl, dr, dt, db});
      EscapeCorridor c;
      c.pad = pi;
      c.net = p.net;
      c.layer = layer;
      c.a = a;
      if (m <= ring) {
        // Perimeter pin: straight out through the nearest side of the package (ties: left, right, top, bottom).
        const auto [hx, hy] = half_extents(p);
        if (m == dl) c.b = {a.x - hx - o.length, a.y};
        else if (m == dr) c.b = {a.x + hx + o.length, a.y};
        else if (m == dt) c.b = {a.x, a.y - hy - o.length};
        else c.b = {a.x, a.y + hy + o.length};
        c.band = std::min(k, pitch / 2 - 1);
        if (stats) ++stats->perimeter;
      } else {
        // Inner ball: dog-bone to the diagonal interstitial site pointing away from the package centre, so all
        // balls of a quadrant fan out the same way and every site serves exactly one ball.
        const Coord sx = a.x >= centre.x ? 1 : -1, sy = a.y >= centre.y ? 1 : -1;
        c.b = {a.x + sx * pitch / 2, a.y + sy * pitch / 2};
        c.via = true;
        c.band = std::min(k, pitch * 35 / 100);
        if (stats) ++stats->dogbones;
      }
      if (c.band <= 0) continue;
      out.push_back(c);
      any = true;
    }
    if (any && stats) ++stats->parts;
  }
  return out;
}

}  // namespace tmk::route
