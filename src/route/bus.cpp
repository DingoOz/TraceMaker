// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/bus.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <stdexcept>

#include "geom/shape.hpp"
#include "route/obstacles.hpp"

namespace tmk::route {

using model::NetId;

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// One index range of a pattern, "[N-M]" or "[N..M]" starting at `i` (the '['); returns its length, 0 if there is none.
std::size_t range_at(const std::string& p, std::size_t i, long& lo, long& hi) {
  if (i >= p.size() || p[i] != '[') return 0;
  std::size_t k = i + 1, a = k;
  while (k < p.size() && is_digit(p[k])) ++k;
  if (k == a || k - a > 9) return 0;
  const std::size_t a_end = k;
  if (k < p.size() && p[k] == '-') ++k;
  else if (k + 1 < p.size() && p[k] == '.' && p[k + 1] == '.') k += 2;
  else return 0;
  const std::size_t c = k;
  while (k < p.size() && is_digit(p[k])) ++k;
  if (k == c || k - c > 9 || k >= p.size() || p[k] != ']') return 0;
  lo = std::stol(p.substr(a, a_end - a));
  hi = std::stol(p.substr(c, k - c));
  return lo <= hi ? k + 1 - i : 0;
}

// Wildcards and index ranges, everything else literal. Backtracking over '*' and over how many digits a range
// takes; net names are short, so this never matters.
bool match_from(const std::string& p, std::size_t pi, const std::string& t, std::size_t ti) {
  while (pi < p.size()) {
    long lo = 0, hi = 0;
    if (const std::size_t n = range_at(p, pi, lo, hi)) {
      long v = 0;
      for (std::size_t k = ti; k < t.size() && is_digit(t[k]) && k - ti < 9; ++k) {
        v = v * 10 + (t[k] - '0');
        if (v >= lo && v <= hi && match_from(p, pi + n, t, k + 1)) return true;
      }
      return false;
    }
    if (p[pi] == '*') {
      for (std::size_t k = ti; k <= t.size(); ++k)
        if (match_from(p, pi + 1, t, k)) return true;
      return false;
    }
    if (ti >= t.size() || (p[pi] != '?' && p[pi] != t[ti])) return false;
    ++pi;
    ++ti;
  }
  return ti == t.size();
}

bool has_range(const std::string& p) {
  long lo = 0, hi = 0;
  for (std::size_t i = 0; i < p.size(); ++i)
    if (range_at(p, i, lo, hi)) return true;
  return false;
}

// Footprints a net has pads on, ascending.
std::vector<int> net_footprints(const model::Board& b, NetId net) {
  std::vector<int> out;
  for (const auto& p : b.pads)
    if (p.net == net && p.footprint >= 0 && p.copper != 0) out.push_back(p.footprint);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

double seg_len(model::Point a, model::Point b) { return std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)); }

}  // namespace

bool natural_less(std::string_view a, std::string_view b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (is_digit(a[i]) && is_digit(b[j])) {
      std::size_t i1 = i, j1 = j;
      while (i1 < a.size() && is_digit(a[i1])) ++i1;
      while (j1 < b.size() && is_digit(b[j1])) ++j1;
      // Compare the two numbers without converting them: longer (after leading zeros) is larger.
      std::size_t ia = i, jb = j;
      while (ia + 1 < i1 && a[ia] == '0') ++ia;
      while (jb + 1 < j1 && b[jb] == '0') ++jb;
      if (i1 - ia != j1 - jb) return i1 - ia < j1 - jb;
      const int c = a.substr(ia, i1 - ia).compare(b.substr(jb, j1 - jb));
      if (c != 0) return c < 0;
      i = i1;
      j = j1;
      continue;
    }
    if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
    ++i;
    ++j;
  }
  if (i < a.size() || j < b.size()) return i == a.size();
  return a < b;  // equal up to leading zeros: plain order, so the relation stays strict
}

bool bus_pattern_match(const std::string& pattern, const std::string& net_name) {
  if (!has_range(pattern)) return model::pattern_match(pattern, net_name);
  return match_from(pattern, 0, net_name, 0);
}

BusGroup bus_from_option(const model::Board& b, const std::string& spec) {
  const auto eq = spec.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) throw std::runtime_error("--bus wants NAME=PATTERN: " + spec);
  BusGroup g;
  g.name = spec.substr(0, eq);
  g.source = "option";
  const std::string pattern = spec.substr(eq + 1);
  std::vector<char> has_pad(b.nets.size(), 0);
  for (const auto& p : b.pads)
    if (p.net > 0 && static_cast<std::size_t>(p.net) < has_pad.size() && p.copper != 0) has_pad[static_cast<std::size_t>(p.net)] = 1;
  for (std::size_t i = 1; i < b.nets.size(); ++i)
    if (has_pad[i] && bus_pattern_match(pattern, b.nets[i].name)) g.members.push_back(static_cast<NetId>(i));
  if (g.members.size() < 2) throw std::runtime_error("--bus " + spec + ": the pattern matches " + std::to_string(g.members.size()) + " net(s) with pads; a bus needs two");
  std::sort(g.members.begin(), g.members.end(), [&](NetId x, NetId y) {
    return natural_less(b.nets[static_cast<std::size_t>(x)].name, b.nets[static_cast<std::size_t>(y)].name);
  });
  return g;
}

std::vector<BusGroup> detect_buses(const model::Board& b, int min_members) {
  // Stem (the name without its trailing index, "A[" for "A[3]") -> nets.
  std::map<std::string, std::vector<NetId>> stems;
  for (std::size_t i = 1; i < b.nets.size(); ++i) {
    const std::string& n = b.nets[i].name;
    if (n.rfind("Net-(", 0) == 0 || n.rfind("unconnected-", 0) == 0) continue;
    std::size_t e = n.size();
    const bool bracket = e > 0 && n[e - 1] == ']';
    if (bracket) --e;
    std::size_t s = e;
    while (s > 0 && is_digit(n[s - 1])) --s;
    if (s == e || e - s > 4) continue;                       // no index, or a number too long to be one
    if (bracket && (s == 0 || n[s - 1] != '[')) continue;
    std::string stem = n.substr(0, s) + (bracket ? "]" : "");
    if (std::none_of(stem.begin(), stem.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; })) continue;
    stems[stem].push_back(static_cast<NetId>(i));
  }
  std::vector<BusGroup> out;
  for (auto& [stem, nets] : stems) {
    if (static_cast<int>(nets.size()) < min_members) continue;
    std::map<NetId, std::vector<int>> fps;
    for (NetId n : nets) fps[n] = net_footprints(b, n);
    std::vector<BusGroup> here;
    // The component pair most of the remaining nets share; then the next, while enough nets are left.
    for (;;) {
      std::map<std::pair<int, int>, int> count;
      for (NetId n : nets) {
        const auto& f = fps[n];
        for (std::size_t x = 0; x < f.size(); ++x)
          for (std::size_t y = x + 1; y < f.size(); ++y) ++count[{f[x], f[y]}];
      }
      std::pair<int, int> best{-1, -1};
      int bc = 0;
      for (const auto& [pr, c] : count)  // ordered map: the first of equal counts wins, the same on every run
        if (c > bc) bc = c, best = pr;
      if (bc < min_members) break;
      BusGroup g;
      g.source = "detected";
      std::vector<NetId> rest;
      for (NetId n : nets) {
        const auto& f = fps[n];
        const bool both = std::binary_search(f.begin(), f.end(), best.first) && std::binary_search(f.begin(), f.end(), best.second);
        (both ? g.members : rest).push_back(n);
      }
      std::sort(g.members.begin(), g.members.end(), [&](NetId x, NetId y) {
        return natural_less(b.nets[static_cast<std::size_t>(x)].name, b.nets[static_cast<std::size_t>(y)].name);
      });
      g.name = "(" + b.footprints[static_cast<std::size_t>(best.first)].reference + "-" + b.footprints[static_cast<std::size_t>(best.second)].reference + ")";
      here.push_back(std::move(g));
      nets = std::move(rest);
    }
    std::string base = stem;
    if (!base.empty() && base.back() == ']') base.resize(base.size() - 2);  // "A[" + "]"
    while (base.size() > 1 && (base.back() == '_' || base.back() == '-' || base.back() == '.')) base.pop_back();
    for (auto& g : here) {
      g.name = here.size() == 1 ? base : base + g.name;
      out.push_back(std::move(g));
    }
  }
  std::sort(out.begin(), out.end(), [](const BusGroup& x, const BusGroup& y) { return natural_less(x.name, y.name); });
  return out;
}

std::optional<Coord> bus_rule_skew(const drc::RuleEngine& re, const BusGroup& g) {
  std::optional<Coord> out;
  for (NetId n : g.members) {
    const auto k = re.net_constraint(n, "skew");
    if (!k || !k->max || std::find(k->items.begin(), k->items.end(), "within_diff_pairs") != k->items.end()) return std::nullopt;
    out = out ? std::min(*out, *k->max) : *k->max;
  }
  return out;
}

Coord ViaHeights::between(int a, int b) const {
  if (a > b) std::swap(a, b);
  if (a < 0 || a == b || static_cast<std::size_t>(b) >= to.size()) return 0;
  return to[static_cast<std::size_t>(b)] - from[static_cast<std::size_t>(a)];
}

ViaHeights via_heights(const model::Board& b, const model::DesignRules& rules) {
  ViaHeights vh;
  const int n = b.copper_count();
  if (!rules.use_height_for_length || n < 2) return vh;
  vh.from.assign(static_cast<std::size_t>(n), 0);
  vh.to.assign(static_cast<std::size_t>(n), 0);
  Coord z = 0;
  // An inner layer counts by half (KiCad halves in integer nanometres), an outer one in full.
  auto copper = [&](int ci, Coord th) {
    const bool outer = ci == 0 || ci == n - 1;
    vh.from[static_cast<std::size_t>(ci)] = outer ? z : z + th - th / 2;
    vh.to[static_cast<std::size_t>(ci)] = outer ? z + th : z + th / 2;
    z += th;
  };
  bool listed = b.stackup.present;
  for (int ci = 0; ci < n && listed; ++ci) listed = b.stackup.position_of_copper(ci) >= 0;
  if (listed) {
    for (const auto& l : b.stackup.layers) {
      if (l.is_copper() && l.copper_index >= 0 && l.copper_index < n) copper(l.copper_index, l.thickness);
      else if (l.name.rfind("dielectric", 0) == 0) z += l.thickness;  // mask, silk and paste are not between copper
    }
    return vh;
  }
  // KiCad's default stackup (BOARD_STACKUP::BuildDefaultStackupList).
  const Coord cu = 35'000, mask = 10'000;
  Coord masks = 0;
  for (const auto& l : b.layers)
    if (l.name == "F.Mask" || l.name == "B.Mask") masks += mask;
  const Coord diel = (b.thickness - cu * n - masks) / (n - 1);
  for (int ci = 0; ci < n; ++ci) {
    copper(ci, cu);
    z += diel;
  }
  return vh;
}

double net_via_length(const model::Board& b, const ViaHeights& vh, NetId net, const std::vector<model::Track>& tracks,
                      const std::vector<model::ArcTrack>& arcs, const std::vector<model::Via>& vias) {
  if (vh.to.empty()) return 0;
  std::vector<std::pair<model::Point, int>> ends;  // track and arc end points of the net, with their layer
  for (const auto& t : tracks)
    if (t.net == net && t.layer >= 0) ends.insert(ends.end(), {{t.a, t.layer}, {t.b, t.layer}});
  for (const auto& a : arcs)
    if (a.net == net && a.layer >= 0) ends.insert(ends.end(), {{a.a, a.layer}, {a.b, a.layer}});
  double sum = 0;
  for (const auto& v : vias) {
    if (v.net != net) continue;
    model::LayerMask span = 0;
    const long double r = static_cast<long double>(v.size) / 2;
    for (const auto& [p, layer] : ends) {
      const long double dx = static_cast<long double>(p.x - v.pos.x), dy = static_cast<long double>(p.y - v.pos.y);
      if (dx * dx + dy * dy <= r * r) span |= model::layer_bit(layer);
    }
    // KiCad takes one pad connected to the via; the first in board order here.
    for (const auto& p : b.pads) {
      if (p.net != net || p.copper == 0) continue;
      const model::Point d = geom::rotate(v.pos - (p.pos + geom::rotate(p.drill_offset, p.angle)), -p.angle);
      if (std::llabs(d.x) > p.size_x / 2 + v.size / 2 || std::llabs(d.y) > p.size_y / 2 + v.size / 2) continue;
      const bool through = p.type == model::PadType::ThruHole || p.type == model::PadType::NpThruHole;
      int side = 0;
      if (through) {
        side = p.footprint >= 0 && b.footprints[static_cast<std::size_t>(p.footprint)].back ? b.copper_count() - 1 : 0;
      } else {
        while (side < b.copper_count() - 1 && !(p.copper & model::layer_bit(side))) ++side;
      }
      span |= model::layer_bit(side);
      break;
    }
    if (span == 0) continue;
    int lo = 0, hi = 0;
    for (int l = 0; l < static_cast<int>(vh.to.size()); ++l)
      if (span & model::layer_bit(l)) hi = l;
    while (!(span & model::layer_bit(lo))) ++lo;
    sum += static_cast<double>(vh.between(lo, hi));
  }
  return sum;
}

BusStats measure_bus(const model::Board& b, const ViaHeights& vh, const std::vector<model::Track>& tracks, const std::vector<model::ArcTrack>& arcs, const std::vector<model::Via>& vias,
                     const std::vector<std::uint8_t>& net_routed, const BusGroup& g) {
  BusStats st;
  std::map<NetId, std::size_t> slot;
  for (NetId n : g.members) {
    slot[n] = st.members.size();
    BusMember m;
    m.net = n;
    m.routed = static_cast<std::size_t>(n) < net_routed.size() && net_routed[static_cast<std::size_t>(n)] != 0;
    st.members.push_back(m);
  }
  for (const auto& t : tracks)
    if (const auto it = slot.find(t.net); it != slot.end()) {
      st.members[it->second].length += seg_len(t.a, t.b);
      if (t.layer >= 0) st.members[it->second].layers |= model::layer_bit(t.layer);
    }
  for (const auto& a : arcs)
    if (const auto it = slot.find(a.net); it != slot.end()) {
      const auto pts = geom::arc_points(a.a, a.mid, a.b);
      for (std::size_t i = 0; i + 1 < pts.size(); ++i) st.members[it->second].length += seg_len(pts[i], pts[i + 1]);
      if (a.layer >= 0) st.members[it->second].layers |= model::layer_bit(a.layer);
    }
  for (const auto& v : vias)
    if (const auto it = slot.find(v.net); it != slot.end()) ++st.members[it->second].vias;
  for (auto& m : st.members) {
    m.via_length = net_via_length(b, vh, m.net, tracks, arcs, vias);
    m.length += m.via_length;
  }
  // Reference: the (vias, layers) most routed members share; the earliest member's among equals.
  std::size_t best = st.members.size();
  int best_n = 0;
  for (std::size_t i = 0; i < st.members.size(); ++i) {
    if (!st.members[i].routed) continue;
    int n = 0;
    for (const auto& o : st.members) n += o.routed && o.vias == st.members[i].vias && o.layers == st.members[i].layers;
    if (n > best_n) best_n = n, best = i;
  }
  bool first = true;
  for (auto& m : st.members) {
    if (!m.routed) continue;
    ++st.routed;
    m.followed = m.vias == st.members[best].vias && m.layers == st.members[best].layers;
    ++(m.followed ? st.followed : st.left);
    st.vias_min = first ? m.vias : std::min(st.vias_min, m.vias);
    st.vias_max = first ? m.vias : std::max(st.vias_max, m.vias);
    st.length_min = first ? m.length : std::min(st.length_min, m.length);
    st.length_max = first ? m.length : std::max(st.length_max, m.length);
    first = false;
  }
  if (best < st.members.size()) {
    st.ref_vias = st.members[best].vias;
    st.ref_layers = st.members[best].layers;
  }
  return st;
}

std::vector<std::uint8_t> nets_joined(const model::Board& b, const model::DesignRules& rules) {
  model::Board copy = b;
  Obstacles obs(copy, rules);
  const auto con = drc::compute_connectivity(copy, obs.copper(), obs.grid());
  std::vector<std::uint8_t> joined(b.nets.size(), 1);
  std::vector<int> root(b.nets.size(), -1);
  const auto& items = obs.copper().items;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];
    if (it.kind != drc::ItemKind::Pad || it.net <= 0 || static_cast<std::size_t>(it.net) >= joined.size()) continue;
    int& r = root[static_cast<std::size_t>(it.net)];
    if (r < 0) r = con.root[i];
    else if (r != con.root[i]) joined[static_cast<std::size_t>(it.net)] = 0;
  }
  return joined;
}

}  // namespace tmk::route
