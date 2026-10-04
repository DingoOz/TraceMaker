#include "route/escape.hpp"

#include "route/obstacles.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>

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

namespace {

Coord neck(const model::DesignRules& r, model::NetId net, const model::Board& b) {
  // As the router's neck-down rung: the board minimum, but not below 0.15 mm unless the minimum is smaller.
  const auto& nc = r.class_for(b.nets[z(net)].name);
  const Coord cw = std::max(nc.track_width, r.minimums.track_width);
  const Coord mn = r.minimums.track_width;
  const Coord w = mn > 0 ? std::max(mn, std::min<Coord>(cw, 150'000)) : std::min<Coord>(cw, 150'000);
  return std::min(w, cw);
}

}  // namespace

std::vector<PartEscape> analyse_escapes(const model::Board& b, const model::DesignRules& r, Obstacles& obs, const EscapeAnalysisOptions& o,
                                        const EscapeOptions& eo) {
  std::vector<PartEscape> out;
  // Pins that must be routed: their net has another pad.
  std::map<model::NetId, int> pads_on_net;
  for (const auto& p : b.pads)
    if (p.net > 0) ++pads_on_net[p.net];
  const int nl = b.copper_count();
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const auto& fp = b.footprints[fi];
    std::vector<int> pads;
    for (int pi : fp.pads) {
      const auto& p = b.pads[z(pi)];
      if (p.copper != 0 && p.type != model::PadType::NpThruHole) pads.push_back(pi);
    }
    if (static_cast<int>(pads.size()) < eo.min_pads) continue;
    Coord pitch = LLONG_MAX;
    for (std::size_t i = 0; i < pads.size(); ++i)
      for (std::size_t j = i + 1; j < pads.size(); ++j) {
        const Point d = b.pads[z(pads[j])].pos - b.pads[z(pads[i])].pos;
        const Coord dist = static_cast<Coord>(std::llround(std::hypot(static_cast<double>(d.x), static_cast<double>(d.y))));
        if (dist > 0) pitch = std::min(pitch, dist);
      }
    if (pitch == LLONG_MAX || pitch > eo.max_pitch) continue;
    Coord x0 = LLONG_MAX, y0 = LLONG_MAX, x1 = LLONG_MIN, y1 = LLONG_MIN;
    for (int pi : pads) {
      const Point q = b.pads[z(pi)].pos;
      x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    PartEscape pe;
    pe.footprint = static_cast<int>(fi);
    pe.ref = fp.reference;
    pe.pitch = pitch;
    // Lattice over the package and its surroundings.
    const Coord L = o.lattice;
    const Coord gx0 = x0 - o.window, gy0 = y0 - o.window;
    const int nx = static_cast<int>((x1 - x0 + 2 * o.window) / L) + 1, ny = static_cast<int>((y1 - y0 + 2 * o.window) / L) + 1;
    auto pt = [&](int ix, int iy) { return Point{gx0 + static_cast<Coord>(ix) * L, gy0 + static_cast<Coord>(iy) * L}; };
    auto outside = [&](Point p) { return p.x < x0 - o.margin || p.x > x1 + o.margin || p.y < y0 - o.margin || p.y > y1 + o.margin; };
    // Codes per (net width class) are cached: pins of the same net class share them.
    std::map<std::pair<Coord, Coord>, std::vector<std::int32_t>> track_codes;  // (half width, rep net) -> layer cells
    std::map<std::pair<Coord, model::NetId>, std::vector<std::int32_t>> via_codes;
    const std::int32_t kUnknown = INT32_MIN;
    bool mask_hint = false;
    for (int pi : pads) {
      const auto& p = b.pads[z(pi)];
      if (p.net <= 0 || pads_on_net[p.net] < 2) continue;
      ++pe.pins;
      const Coord hw = neck(r, p.net, b) / 2;
      const auto& nc = r.class_for(b.nets[z(p.net)].name);
      // The smallest via the router may use (its neck-down rung): what the board minimums allow, drill >= 0.2 mm.
      const Coord cdrill = std::max(nc.via_drill, r.minimums.through_hole_diameter);
      const Coord cdia = std::max({nc.via_diameter, r.minimums.via_diameter, cdrill + 2 * r.minimums.via_annular_width});
      const Coord drill = std::min(cdrill, std::max<Coord>(r.minimums.through_hole_diameter, 200'000));
      const Coord dia = std::min(cdia, std::max(r.minimums.via_diameter, drill + 2 * std::max<Coord>(r.minimums.via_annular_width, 100'000)));
      const Coord clr = std::max(nc.clearance, r.minimums.clearance);
      auto& tc = track_codes[{hw, clr}];
      if (tc.empty()) tc.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), kUnknown);
      auto& vc = via_codes[{dia * 1'000'000 + drill, static_cast<model::NetId>(clr / 1000)}];
      if (vc.empty()) vc.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), kUnknown);
      auto ok = [&](std::int32_t c) { return c == Obstacles::kFree || c == p.net; };
      auto tcode = [&](int l, int ix, int iy) {
        auto& c = tc[(static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix)];
        if (c == kUnknown) c = obs.fixed_code(pt(ix, iy), l, hw, 0, p.net);
        return c;
      };
      auto vcode = [&](int ix, int iy) {
        auto& c = vc[static_cast<std::size_t>(iy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix)];
        if (c == kUnknown) c = obs.fixed_via_code(pt(ix, iy), dia, drill, 0, p.net);
        return c;
      };
      // Start: lattice points inside the pad on its layers (pad copper is the pin's own net).
      std::vector<std::uint8_t> seen(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
      std::deque<std::tuple<int, int, int>> q;
      const auto [hx, hy] = half_extents(p);
      for (int l = 0; l < nl; ++l) {
        if (!(p.copper & (model::LayerMask{1} << l))) continue;
        const int ix0 = static_cast<int>((p.pos.x - hx - gx0) / L), ix1 = static_cast<int>((p.pos.x + hx - gx0) / L) + 1;
        const int iy0 = static_cast<int>((p.pos.y - hy - gy0) / L), iy1 = static_cast<int>((p.pos.y + hy - gy0) / L) + 1;
        bool any = false;
        for (int iy = std::max(0, iy0); iy <= std::min(ny - 1, iy1); ++iy)
          for (int ix = std::max(0, ix0); ix <= std::min(nx - 1, ix1); ++ix)
            if (ok(tcode(l, ix, iy))) {
              const std::size_t k = (static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix);
              if (!seen[k]) {
                seen[k] = 1;
                q.emplace_back(l, ix, iy);
                any = true;
              }
            }
        if (!any) {  // pad smaller than the lattice: its nearest point
          const int ix = std::clamp(static_cast<int>((p.pos.x - gx0 + L / 2) / L), 0, nx - 1), iy = std::clamp(static_cast<int>((p.pos.y - gy0 + L / 2) / L), 0, ny - 1);
          seen[(static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix)] = 1;
          q.emplace_back(l, ix, iy);
        }
      }
      bool escaped = false, via_seen = false;
      std::vector<std::pair<int, int>> region;  // reached points (for the mask hint)
      static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
      while (!q.empty() && !escaped) {
        const auto [l, ix, iy] = q.front();
        q.pop_front();
        if (outside(pt(ix, iy))) {
          escaped = true;
          break;
        }
        if (region.size() < 20'000) region.emplace_back(ix, iy);
        for (int d = 0; d < 8; ++d) {
          const int jx = ix + kDx[d], jy = iy + kDy[d];
          if (jx < 0 || jy < 0 || jx >= nx || jy >= ny) continue;
          const std::size_t k = (static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(jy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(jx);
          if (seen[k] || !ok(tcode(l, jx, jy))) continue;
          seen[k] = 1;
          q.emplace_back(l, jx, jy);
        }
        if (nl > 1 && ok(vcode(ix, iy))) {
          via_seen = true;
          for (int l2 = 0; l2 < nl; ++l2) {
            const std::size_t k = (static_cast<std::size_t>(l2) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(iy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix);
            if (seen[k]) continue;
            seen[k] = 1;
            q.emplace_back(l2, ix, iy);
          }
        }
      }
      if (escaped) {
        ++pe.escapable;
        continue;
      }
      DeadPin dp;
      dp.pad = pi;
      if (via_seen) {
        dp.reason = "trapped on every layer it can reach";
      } else {
        // Would a via fit somewhere it can reach if vias were tented (no solder-mask opening of their own)?
        bool mask_only = false;
        if (obs.via_mask() > 0 && nl > 1) {
          const Coord saved = obs.via_mask();
          obs.set_via_mask(0);
          for (const auto& [ix, iy] : region)
            if (ok(obs.fixed_via_code(pt(ix, iy), dia, drill, 0, p.net))) {
              mask_only = true;
              break;
            }
          obs.set_via_mask(saved);
        }
        if (mask_only) {
          dp.reason = "no via site: only the solder-mask rule blocks them (untented vias)";
          mask_hint = true;
        } else {
          dp.reason = nl > 1 ? "no channel at " + std::to_string(nm_to_mm(2 * hw)).substr(0, 5) + " mm and no via site within reach"
                             : "no channel at " + std::to_string(nm_to_mm(2 * hw)).substr(0, 5) + " mm";
        }
      }
      pe.dead.push_back(dp);
    }
    if (pe.pins == 0) continue;
    if (mask_hint)
      pe.hint = "tent the vias (board setup) or reduce pad_to_mask_clearance (" + std::to_string(nm_to_mm(obs.via_mask())).substr(0, 5) +
                " mm): vias would then fit where the dead pins need them";
    out.push_back(std::move(pe));
  }
  return out;
}

}  // namespace tmk::route
