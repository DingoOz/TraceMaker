// SPDX-License-Identifier: GPL-3.0-or-later
#include "crules/topology.hpp"

#include <algorithm>
#include <regex>

#include "crules/names.hpp"
#include "geom/shape.hpp"

namespace tmk::crules {

namespace {

bool digit_after(std::string_view ref, std::size_t n) {
  return ref.size() > n && std::isdigit(static_cast<unsigned char>(ref[n])) != 0;
}

// IC pin names that mark supply and ground pins (generalised decoupling, doc 15 ic_decoupling pin_name detector).
bool supply_pin_name(const std::string& f) {
  static const std::regex re("^(vdd|vcc|vddio|vdda|avdd|avcc|dvdd|vref|vcap|vbat|vio|vddcore|vccio|vccint|vccaux)", std::regex::icase | std::regex::optimize);
  return !f.empty() && std::regex_search(f, re);
}
bool ground_pin_name(const std::string& f) {
  static const std::regex re("^(gnd|vss|agnd|dgnd|pgnd|gnda|gndd|vssa|avss|dvss)\\d*$", std::regex::icase | std::regex::optimize);
  return !f.empty() && std::regex_search(f, re);
}

}  // namespace

BoardIndex::BoardIndex(const model::Board& b) : b_(b) {
  net_pads_.resize(b.nets.size());
  fp_nets_.resize(b.footprints.size());
  letters_.resize(b.footprints.size());
  ground_.assign(b.nets.size(), 0);
  supply_.assign(b.nets.size(), 0);
  for (std::size_t i = 0; i < b.pads.size(); ++i) {
    const auto& p = b.pads[i];
    if (!p.pinfunction.empty()) has_pin_names_ = true;
    if (p.net > 0 && static_cast<std::size_t>(p.net) < b.nets.size()) net_pads_[static_cast<std::size_t>(p.net)].push_back(static_cast<int>(i));
  }
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const auto& fp = b.footprints[fi];
    letters_[fi] = ref_letters(fp.reference);
    auto& v = fp_nets_[fi];
    for (int pi : fp.pads)
      if (const auto n = b.pads[static_cast<std::size_t>(pi)].net; n > 0) v.push_back(n);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }
  for (std::size_t n = 1; n < b.nets.size(); ++n) {
    const std::string& name = b.nets[n].name;
    bool gnd = ground_like_name(name), sup = power_like_name(name) || analog_supply_name(name);
    for (int pi : net_pads_[n]) {
      const auto& p = b.pads[static_cast<std::size_t>(pi)];
      if (p.pinfunction.empty() || !is_ic(p.footprint)) continue;
      gnd = gnd || ground_pin_name(p.pinfunction);
      sup = sup || supply_pin_name(p.pinfunction);
    }
    ground_[n] = gnd ? 1 : 0;
    supply_[n] = (sup && !gnd) ? 1 : 0;
  }
}

std::vector<int> BoardIndex::parts_on(model::NetId n) const {
  std::vector<int> v;
  if (n <= 0) return v;
  for (int pi : net_pads(n)) v.push_back(b_.pads[static_cast<std::size_t>(pi)].footprint);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

int BoardIndex::pad_on(int fi, model::NetId n) const {
  for (int pi : b_.footprints[static_cast<std::size_t>(fi)].pads)
    if (b_.pads[static_cast<std::size_t>(pi)].net == n) return pi;
  return -1;
}

int BoardIndex::pad_count(int fi) const {
  int k = 0;
  for (int pi : b_.footprints[static_cast<std::size_t>(fi)].pads) {
    const auto& p = b_.pads[static_cast<std::size_t>(pi)];
    k += (p.net > 0 || p.copper != 0) ? 1 : 0;
  }
  return k;
}

bool BoardIndex::is_ic(int fi) const {
  const std::string& r = b_.footprints[static_cast<std::size_t>(fi)].reference;
  const std::string& l = letters(fi);
  return (l == "U" && digit_after(r, 1)) || (l == "IC" && digit_after(r, 2));
}

bool BoardIndex::is_regulator_ref(int fi) const {
  const std::string& l = letters(fi);
  const std::string& r = b_.footprints[static_cast<std::size_t>(fi)].reference;
  return is_ic(fi) || ((l == "VR" || l == "REG" || l == "PS") && digit_after(r, l.size()));
}

bool BoardIndex::is_cap(int fi) const {
  const auto& fp = b_.footprints[static_cast<std::size_t>(fi)];
  return letters(fi) == "C" && digit_after(fp.reference, 1) && fp.pads.size() == 2;
}

bool BoardIndex::is_series(int fi) const {
  const auto& fp = b_.footprints[static_cast<std::size_t>(fi)];
  const std::string& l = letters(fi);
  return fp.pads.size() == 2 && digit_after(fp.reference, l.size()) && (l == "R" || l == "FB" || l == "L");
}

bool BoardIndex::is_connector(int fi) const {
  const std::string r = upper(b_.footprints[static_cast<std::size_t>(fi)].reference);
  if ((r.starts_with("J") || r.starts_with("P")) && digit_after(r, 1)) return true;
  return r.starts_with("CN") || r.starts_with("CON") || r.starts_with("USB") || r.starts_with("XS");
}

std::vector<DecapTie> decap_ties(const model::Board& b, const std::function<bool(int)>& usable, bool generalised) {
  std::vector<DecapTie> out;
  const BoardIndex ix(b);
  auto supply = [&](model::NetId n) {
    if (!generalised) return power_like_name(b.nets[static_cast<std::size_t>(n)].name) && !ground_like_name(b.nets[static_cast<std::size_t>(n)].name);
    return ix.supply(n);
  };
  auto ground = [&](model::NetId n) { return generalised ? ix.ground(n) : ground_like_name(b.nets[static_cast<std::size_t>(n)].name); };
  auto ic = [&](int fi) { return generalised ? ix.is_regulator_ref(fi) : ix.is_ic(fi); };
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const int f = static_cast<int>(fi);
    if (!usable(f) || !ix.is_cap(f)) continue;
    const auto& fp = b.footprints[fi];
    const auto& p0 = b.pads[static_cast<std::size_t>(fp.pads[0])];
    const auto& p1 = b.pads[static_cast<std::size_t>(fp.pads[1])];
    if (p0.net <= 0 || p1.net <= 0 || p0.net == p1.net) continue;
    const int sp = supply(p0.net) && ground(p1.net) ? fp.pads[0] : supply(p1.net) && ground(p0.net) ? fp.pads[1] : -1;
    if (sp < 0) continue;
    const auto& s = b.pads[static_cast<std::size_t>(sp)];
    int best = -1;
    geom::i128 best_d = 0;
    for (int k : ix.net_pads(s.net)) {  // ascending pad index: ties keep the first pad
      const auto& q = b.pads[static_cast<std::size_t>(k)];
      if (q.footprint < 0 || q.footprint == f || !usable(q.footprint) || !ic(q.footprint)) continue;
      const geom::i128 dx = q.pos.x - s.pos.x, dy = q.pos.y - s.pos.y, d = dx * dx + dy * dy;
      if (best < 0 || d < best_d) best = k, best_d = d;
    }
    if (best >= 0) out.push_back({f, sp, best});
  }
  return out;
}

namespace {

// Position of i in the bit-reversal permutation (van der Corput order): every prefix of the order is spread evenly
// over the range, so k picks out of m land about m/k apart.
std::uint32_t bit_reversed(std::uint32_t i, int bits) {
  std::uint32_t r = 0;
  for (int k = 0; k < bits; ++k) r |= ((i >> k) & 1U) << (bits - 1 - k);
  return r;
}

}  // namespace

int spread_unplaced_decaps(const model::Board& b, std::vector<DecapTie>& ties, const std::function<bool(int)>& usable,
                           const std::function<bool(int)>& unplaced) {
  const BoardIndex ix(b);
  auto net_of = [&](const DecapTie& t) { return b.pads[static_cast<std::size_t>(t.cap_pad)].net; };
  // Supply nets that carry an unplaced cap, ascending (deterministic, no hash order).
  std::vector<model::NetId> nets;
  for (const auto& t : ties)
    if (unplaced(t.cap_fp)) nets.push_back(net_of(t));
  std::sort(nets.begin(), nets.end());
  nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
  int changed = 0;
  for (const model::NetId n : nets) {
    // Candidate pads: IC pads on the net, ascending pad index (footprint by footprint, round each package).
    std::vector<int> cand;
    for (int k : ix.net_pads(n)) {
      const int f = b.pads[static_cast<std::size_t>(k)].footprint;
      if (f >= 0 && usable(f) && ix.is_ic(f)) cand.push_back(k);
    }
    if (cand.empty()) continue;
    int bits = 0;
    while ((std::size_t{1} << bits) < cand.size()) ++bits;
    std::vector<int> load(cand.size(), 0);  // caps tied to each candidate pad so far
    std::vector<std::uint32_t> rank(cand.size());
    for (std::size_t i = 0; i < cand.size(); ++i) rank[i] = bit_reversed(static_cast<std::uint32_t>(i), bits);
    for (const auto& t : ties)  // the designer's caps hold their pads first
      if (!unplaced(t.cap_fp) && net_of(t) == n)
        if (const auto it = std::lower_bound(cand.begin(), cand.end(), t.ic_pad); it != cand.end() && *it == t.ic_pad)
          ++load[static_cast<std::size_t>(it - cand.begin())];
    for (auto& t : ties) {  // ascending capacitor footprint index (decap_ties' order)
      if (!unplaced(t.cap_fp) || net_of(t) != n) continue;
      std::size_t best = 0;
      for (std::size_t i = 1; i < cand.size(); ++i)
        if (load[i] < load[best] || (load[i] == load[best] && rank[i] < rank[best])) best = i;
      ++load[best];
      if (t.ic_pad != cand[best]) ++changed;
      t.ic_pad = cand[best];
    }
  }
  return changed;
}

}  // namespace tmk::crules
