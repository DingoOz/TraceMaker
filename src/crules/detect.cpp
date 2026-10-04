#include "crules/detect.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

#include "crules/names.hpp"
#include "geom/shape.hpp"

namespace tmk::crules {

const Role* Instance::role(std::string_view name) const {
  for (const auto& r : roles)
    if (r.name == name) return &r;
  return nullptr;
}

const RuleOverride* Instance::override_for(std::string_view rule) const {
  for (const auto& o : overrides)
    if (o.rule == rule) return &o;
  return nullptr;
}

bool Instance::disabled(std::string_view rule) const {
  const RuleOverride* o = override_for(rule);
  return o && o->disabled;
}

const nlohmann::ordered_json& rule_params(const Instance& in, const RuleSpec& r) {
  const RuleOverride* o = in.override_for(r.id);
  return o && o->has_params ? o->params : r.params;
}

namespace {

using model::NetId;

// What a category's binder found for one anchor (doc 15 §3.3).
struct Bound {
  bool ok = true;                 // false: the anchor cannot be this category (reported as possible, with `why`)
  std::string why;
  std::vector<std::string> topology;  // topology detectors that matched
  // The topology match rests on net structure only (no pin names): it counts only when a part-level detector
  // (lib_id, value, keywords) also matched, so an LED driven from two MCU pins is not a crystal.
  bool topology_weak = false;
  std::vector<int> pin_pads;      // pads the pin_name detectors test instead of the anchor's own (decoupling)
  std::vector<Role> roles;
};

struct Ctx {
  const model::Board& b;
  const BoardIndex& ix;
  std::map<int, DecapTie> decap;  // cap footprint -> generalised D25 tie (ordered map: deterministic)
};

std::regex icase(const char* p) { return std::regex(p, std::regex::ECMAScript | std::regex::icase | std::regex::optimize); }
bool search(const std::string& s, const std::regex& re) { return !s.empty() && std::regex_search(s, re); }

Role make_role(std::string name) {
  Role r;
  r.name = std::move(name);
  return r;
}

void add_unique(std::vector<int>& v, int x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}
void add_unique_net(std::vector<NetId>& v, NetId x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

geom::i128 dist2(Point a, Point c) {
  const geom::i128 dx = a.x - c.x, dy = a.y - c.y;
  return dx * dx + dy * dy;
}

// Nets reached from `n` through one series element (two-pad R/FB/L), excluding footprints in `skip`.
std::vector<NetId> across_series(const Ctx& c, NetId n, const std::vector<int>& skip) {
  std::vector<NetId> out;
  for (int f : c.ix.parts_on(n)) {
    if (!c.ix.is_series(f) || std::find(skip.begin(), skip.end(), f) != skip.end()) continue;
    for (NetId m : c.ix.fp_nets(f))
      if (m != n && !c.ix.ground(m)) add_unique_net(out, m);
  }
  return out;
}

// ICs on net n, directly or across one series element.
std::vector<int> ics_near(const Ctx& c, NetId n, const std::vector<int>& skip) {
  std::vector<int> out;
  std::vector<NetId> nets = {n};
  for (NetId m : across_series(c, n, skip)) nets.push_back(m);
  for (NetId m : nets)
    for (int f : c.ix.parts_on(m))
      if (c.ix.is_ic(f) && std::find(skip.begin(), skip.end(), f) == skip.end()) add_unique(out, f);
  std::sort(out.begin(), out.end());
  return out;
}

// Two-pad capacitors from net n to a ground net.
std::vector<int> caps_to_ground(const Ctx& c, NetId n) {
  std::vector<int> out;
  for (int f : c.ix.parts_on(n)) {
    if (!c.ix.is_cap(f)) continue;
    const auto& fp = c.b.footprints[static_cast<std::size_t>(f)];
    const NetId a = c.b.pads[static_cast<std::size_t>(fp.pads[0])].net, d = c.b.pads[static_cast<std::size_t>(fp.pads[1])].net;
    if ((a == n && c.ix.ground(d)) || (d == n && c.ix.ground(a))) out.push_back(f);
  }
  return out;
}

// Capacitors on net n (to ground) that belong to footprint `owner`: in the input placement the nearest pad on n
// of an active part (IC, regulator reference, or `owner` itself) is one of owner's pads (the D25 nearest-pin
// rule, used so a rail's decoupling caps are not all claimed by the regulator that feeds the rail).
std::vector<int> own_caps(const Ctx& c, NetId n, int owner) {
  std::vector<int> out;
  if (n <= 0) return out;
  for (int cap : caps_to_ground(c, n)) {
    const int sp = c.ix.pad_on(cap, n);
    const Point at = c.b.pads[static_cast<std::size_t>(sp)].pos;
    int best = -1;
    geom::i128 bd = 0;
    for (int k : c.ix.net_pads(n)) {
      const int f = c.b.pads[static_cast<std::size_t>(k)].footprint;
      if (f == cap || !(f == owner || c.ix.is_regulator_ref(f))) continue;
      const geom::i128 d = dist2(c.b.pads[static_cast<std::size_t>(k)].pos, at);
      if (best < 0 || d < bd) best = k, bd = d;
    }
    if (best >= 0 && c.b.pads[static_cast<std::size_t>(best)].footprint == owner) out.push_back(cap);
  }
  return out;
}

const std::string& ref(const Ctx& c, int f) { return c.b.footprints[static_cast<std::size_t>(f)].reference; }

// ---- USB (usb2, usb3_typec) ------------------------------------------------------------------------------------

bool dp_name(const std::string& leaf) {
  static const std::regex a = icase(R"((^|[_\-.])(D\+|DP|D_P|DPLUS|DATA\+|USBDP|USB_?D_?P)$)");
  static const std::regex b = icase(R"(^USB.*(D\+|DP|D_P|_P|\+)$)");
  return std::regex_search(leaf, a) || std::regex_search(leaf, b);
}
bool dm_name(const std::string& leaf) {
  static const std::regex a = icase(R"((^|[_\-.])(D-|DM|D_M|D_N|DN|DMINUS|DATA-|USBDM|USB_?D_?[MN])$)");
  static const std::regex b = icase(R"(^USB.*(D-|DM|D_M|D_N|_N|_M|-)$)");
  return std::regex_search(leaf, a) || std::regex_search(leaf, b);
}

Bound bind_usb(const Ctx& c, int anchor, bool typec_only) {
  Bound r;
  const auto& fp = c.b.footprints[static_cast<std::size_t>(anchor)];
  static const std::regex lib_c = icase("usb[_-]?c([_-]|$)|type.?c");
  static const std::regex lib_ab = icase("usb_(micro|mini|a|b)|micro.?usb|mini.?usb|usb.?(micro|mini)|usb_?a_|usb_?b_");
  const std::string fname = fp.lib_id.substr(fp.lib_id.rfind(':') == std::string::npos ? 0 : fp.lib_id.rfind(':') + 1);
  const bool is_c = search(fname, lib_c), is_ab = search(fname, lib_ab);
  if (typec_only && !is_c) {
    // usb3_typec also matches lib_id "usb3"; anything else is a plain USB 2.0 connector.
    static const std::regex usb3 = icase("usb3|usb_?3");
    if (!search(fname, usb3)) {
      r.ok = false;
      r.why = "not a Type-C or USB 3 receptacle";
      return r;
    }
  }
  Role conn = make_role("connector");
  conn.parts.push_back(anchor);
  Role dp = make_role("dp"), dm = make_role("dm"), vbus = make_role("vbus");
  static const std::regex pin_dp = icase(R"(^(d\+|dp|usb_?dp|usb_?d\+|dp\d)$)"), pin_dm = icase(R"(^(d-|dm|usb_?dm|usb_?d-|dn\d?|dm\d)$)");
  static const std::regex pin_vbus = icase("^vbus");
  // Pin names first (KiCad 6+), then net names, then the standard pad numbers of KiCad's USB footprints.
  for (int pi : fp.pads) {
    const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
    if (p.net <= 0) continue;
    if (search(p.pinfunction, pin_dp)) add_unique_net(dp.nets, p.net), add_unique(dp.pads, pi);
    if (search(p.pinfunction, pin_dm)) add_unique_net(dm.nets, p.net), add_unique(dm.pads, pi);
    if (search(p.pinfunction, pin_vbus)) add_unique_net(vbus.nets, p.net);
  }
  if (dp.nets.empty() || dm.nets.empty()) {
    dp = make_role("dp");
    dm = make_role("dm");
    for (int pi : fp.pads) {
      const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
      if (p.net <= 0) continue;
      const std::string leaf(net_leaf(c.ix.net_name(p.net)));
      if (dp_name(leaf) && !dm_name(leaf)) add_unique_net(dp.nets, p.net), add_unique(dp.pads, pi);
      else if (dm_name(leaf) && !dp_name(leaf)) add_unique_net(dm.nets, p.net), add_unique(dm.pads, pi);
    }
  }
  if ((dp.nets.empty() || dm.nets.empty()) && (is_c || is_ab)) {
    dp = make_role("dp");
    dm = make_role("dm");
    for (int pi : fp.pads) {
      const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
      if (p.net <= 0 || !c.ix.signal(p.net)) continue;
      const std::string& n = p.number;
      if (is_c ? (n == "A6" || n == "B6") : n == "3") add_unique_net(dp.nets, p.net), add_unique(dp.pads, pi);
      if (is_c ? (n == "A7" || n == "B7") : n == "2") add_unique_net(dm.nets, p.net), add_unique(dm.pads, pi);
    }
  }
  if (vbus.nets.empty())
    for (NetId n : c.ix.fp_nets(anchor)) {
      static const std::regex vb = icase("vbus");
      if (search(c.ix.net_name(n), vb)) add_unique_net(vbus.nets, n);
    }
  if (vbus.nets.empty() && (is_c || is_ab))
    for (int pi : fp.pads) {
      const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
      const std::string& n = p.number;
      if (p.net > 0 && c.ix.supply(p.net) && (is_c ? (n == "A4" || n == "A9" || n == "B4" || n == "B9") : n == "1")) add_unique_net(vbus.nets, p.net);
    }
  r.roles.push_back(conn);
  // A pair needs two distinct nets; otherwise the roles stay unbound and the pair rules are reported as such.
  const bool pair = !dp.nets.empty() && !dm.nets.empty() && dp.nets != dm.nets;
  if (pair) {
    r.roles.push_back(dp);
    r.roles.push_back(dm);
    std::vector<NetId> data = dp.nets;
    for (NetId n : dm.nets) add_unique_net(data, n);
    // ESD: a part (not a passive, not a connector) with a pad on D+ or D- and a pad on ground.
    Role esd = make_role("esd"), cmc = make_role("cmc"), xcvr = make_role("transceiver");
    std::vector<int> cands;
    for (NetId n : data)
      for (int f : c.ix.parts_on(n)) add_unique(cands, f);
    std::sort(cands.begin(), cands.end());
    for (int f : cands) {
      if (f == anchor || c.ix.is_connector(f)) continue;
      const std::string& l = c.ix.letters(f);
      if (l == "R" || l == "C" || l == "FB") continue;
      bool on_gnd = false, on_d = false, on_dp = false, on_dm = false, other_signal = false;
      for (NetId n : c.ix.fp_nets(f)) {
        on_gnd |= c.ix.ground(n);
        const bool p = std::find(dp.nets.begin(), dp.nets.end(), n) != dp.nets.end();
        const bool m = std::find(dm.nets.begin(), dm.nets.end(), n) != dm.nets.end();
        on_dp |= p;
        on_dm |= m;
        other_signal |= c.ix.signal(n) && !p && !m;
      }
      on_d = on_dp || on_dm;
      // A protection part touches only the data lines, supplies and ground (an MCU has other signals).
      if (on_d && on_gnd && !other_signal && c.ix.pad_count(f) <= 10) esd.parts.push_back(f);
      else if (on_dp && on_dm && c.ix.pad_count(f) == 4 && !on_gnd && (l == "L" || l == "FL" || l == "T" || l == "CMC")) cmc.parts.push_back(f);
    }
    // Transceiver: the IC with the most pads on the data nets (directly or across a series resistor), not an ESD part.
    std::vector<int> skip = esd.parts;
    skip.push_back(anchor);
    int best = -1, best_pads = -1;
    for (NetId n : data)
      for (int f : ics_near(c, n, skip)) {
        const int np = c.ix.pad_count(f);
        if (np > best_pads || (np == best_pads && f < best)) best = f, best_pads = np;
      }
    if (best >= 0) xcvr.parts.push_back(best);
    if (!esd.parts.empty()) r.roles.push_back(esd);
    if (!cmc.parts.empty()) r.roles.push_back(cmc);
    if (!xcvr.parts.empty()) r.roles.push_back(xcvr);
  }
  if (!vbus.nets.empty()) r.roles.push_back(vbus);
  if (is_c) {
    Role cc = make_role("cc");
    static const std::regex pin_cc = icase("^cc[12]$");
    for (int pi : fp.pads) {
      const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
      if (p.net > 0 && (search(p.pinfunction, pin_cc) || ((p.number == "A5" || p.number == "B5") && c.ix.signal(p.net)))) add_unique_net(cc.nets, p.net);
    }
    if (!cc.nets.empty()) r.roles.push_back(cc);
  }
  return r;
}

// ---- Clocks ----------------------------------------------------------------------------------------------------

Bound bind_crystal(const Ctx& c, int anchor) {
  Bound r;
  std::vector<NetId> sig;
  for (NetId n : c.ix.fp_nets(anchor))
    if (c.ix.signal(n)) sig.push_back(n);
  if (sig.size() != 2) {
    r.ok = false;
    r.why = "needs exactly two oscillator nets, has " + std::to_string(sig.size());
    return r;
  }
  Role xtal = make_role("crystal");
  xtal.parts.push_back(anchor);
  xtal.nets = sig;
  r.roles.push_back(xtal);
  // The IC that both crystal nets reach (directly or across one series resistor). Most pads wins, then lowest index.
  const std::vector<int> skip = {anchor};
  const auto a = ics_near(c, sig[0], skip), d = ics_near(c, sig[1], skip);
  int ic = -1, best_pads = -1;
  for (int f : a)
    if (std::find(d.begin(), d.end(), f) != d.end() && c.ix.pad_count(f) > best_pads) ic = f, best_pads = c.ix.pad_count(f);
  Role xin = make_role("xin"), xout = make_role("xout"), icr = make_role("ic"), caps = make_role("load_caps"), ser = make_role("series_r");
  if (ic >= 0) {
    static const std::regex osc = icase(R"(osc|xtal|xt|^x(in|out|i|o|1|2)$|^xc(in|out)|^xi$|^xo$|clk_?in|tosc|xin|xout)");
    static const std::regex out_re = icase(R"(out|xo$|x2|xtal2|_n$|^xo|xc2)");
    icr.parts.push_back(ic);
    std::vector<int> pins;
    for (NetId n : sig) {
      std::vector<NetId> nets = {n};
      for (NetId m : across_series(c, n, skip)) nets.push_back(m);
      for (NetId m : nets)
        if (const int p = c.ix.pad_on(ic, m); p >= 0) pins.push_back(p);
      for (int f : c.ix.parts_on(n))
        if (c.ix.is_series(f) && c.ix.letters(f) == "R" && f != anchor) add_unique(ser.parts, f);
    }
    std::sort(pins.begin(), pins.end());
    bool named = !pins.empty();
    for (int p : pins) named = named && search(c.b.pads[static_cast<std::size_t>(p)].pinfunction, osc);
    // Topology two_terminal_between_osc_pins: both nets end on one IC's oscillator pins. Old boards have no pin
    // names: both nets ending on the same IC is accepted as the same evidence (doc 15 §3.6 caps nothing here,
    // the crystal category has no pin_name detector; see dev/assumptions-m13.md).
    if (named || !c.ix.has_pin_names()) r.topology.push_back("two_terminal_between_osc_pins");
    r.topology_weak = !named;
    for (int p : pins) {
      const bool is_out = c.ix.has_pin_names() ? search(c.b.pads[static_cast<std::size_t>(p)].pinfunction, out_re) : (p == pins.back() && pins.size() > 1);
      (is_out ? xout : xin).pads.push_back(p);
      add_unique_net(is_out ? xout.nets : xin.nets, c.b.pads[static_cast<std::size_t>(p)].net);
    }
  }
  for (NetId n : sig)
    for (int f : caps_to_ground(c, n)) add_unique(caps.parts, f);
  std::sort(caps.parts.begin(), caps.parts.end());
  for (Role* x : {&icr, &xin, &xout, &caps, &ser})
    if (!x->empty()) r.roles.push_back(*x);
  return r;
}

Bound bind_oscillator(const Ctx& c, int anchor) {
  Bound r;
  Role osc = make_role("osc"), out = make_role("out"), load = make_role("load"), decap = make_role("decap");
  osc.parts.push_back(anchor);
  r.roles.push_back(osc);
  const std::vector<int> skip = {anchor};
  for (NetId n : c.ix.fp_nets(anchor)) {
    if (!c.ix.signal(n)) continue;
    const auto ics = ics_near(c, n, skip);
    if (ics.empty()) continue;
    out.nets.push_back(n);
    for (int f : ics) add_unique(load.parts, f);
  }
  for (NetId n : c.ix.fp_nets(anchor))
    if (c.ix.supply(n))
      for (int f : own_caps(c, n, anchor)) add_unique(decap.parts, f);
  for (Role* x : {&out, &load, &decap})
    if (!x->empty()) r.roles.push_back(*x);
  return r;
}

// ---- Power -----------------------------------------------------------------------------------------------------

Bound bind_decoupling(const Ctx& c, int anchor) {
  Bound r;
  const auto it = c.decap.find(anchor);
  if (it == c.decap.end()) {
    r.ok = false;
    r.why = "not a capacitor between a supply and ground shared with an IC";
    return r;
  }
  const DecapTie& t = it->second;
  r.topology.push_back("two_terminal_cap_supply_to_ground_sharing_ic_power_pin");
  r.pin_pads.push_back(t.ic_pad);
  Role cap = make_role("cap"), pin = make_role("pin"), ic = make_role("ic");
  cap.parts.push_back(anchor);
  cap.pads.push_back(t.cap_pad);
  pin.pads.push_back(t.ic_pad);
  const auto& icp = c.b.pads[static_cast<std::size_t>(t.ic_pad)];
  pin.nets.push_back(icp.net);
  ic.parts.push_back(icp.footprint);
  r.roles.push_back(cap);
  r.roles.push_back(pin);
  r.roles.push_back(ic);
  // DEC-07: reference and core-regulator pins want the cap closer (3 mm instead of 5 mm).
  static const std::regex vref = icase("vref|aref|vcap|^ref$");
  if (search(icp.pinfunction, vref) || search(std::string(net_leaf(c.ix.net_name(icp.net))), vref)) {
    Role v = make_role(search(icp.pinfunction, icase("vcap")) ? "vcap" : "vref");
    v.pads.push_back(t.ic_pad);
    r.roles.push_back(v);
  }
  if (const auto pf = capacitance_pf(c.b.footprints[static_cast<std::size_t>(anchor)].value); pf && *pf >= 1'000'000) {
    Role bulk = make_role("bulk");
    bulk.parts.push_back(anchor);
    r.roles.push_back(bulk);
  }
  return r;
}

// Regulator input/output nets: pin names first, else supply-net names (higher voltage = input).
void regulator_rails(const Ctx& c, int anchor, NetId& vin, NetId& vout, NetId& sw, NetId& fb) {
  static const std::regex in_re = icase(R"(^(vin|in|vi|pvin|vcc|vbat|vsys)\d*$)"), out_re = icase(R"(^(vout|out|vo|output)\d*$)");
  static const std::regex sw_re = icase(R"(^(sw|lx|ph)\d*$)"), fb_re = icase(R"(^(fb|vfb|adj|vos)$)");
  vin = vout = sw = fb = 0;
  for (int pi : c.b.footprints[static_cast<std::size_t>(anchor)].pads) {
    const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
    if (p.net <= 0 || p.pinfunction.empty()) continue;
    if (!vin && search(p.pinfunction, in_re)) vin = p.net;
    else if (!vout && search(p.pinfunction, out_re)) vout = p.net;
    else if (!sw && search(p.pinfunction, sw_re)) sw = p.net;
    else if (!fb && search(p.pinfunction, fb_re)) fb = p.net;
  }
  if (vin && vout) return;
  std::vector<NetId> rails;
  for (NetId n : c.ix.fp_nets(anchor))
    if (c.ix.supply(n)) rails.push_back(n);
  if (rails.size() < 2) return;
  // Two or more supply nets: by voltage in the name, else by input-like names.
  static const std::regex in_name = icase("vin|vbus|vbat|raw|vsys|in$|_in|input|vcc_?in|12v|24v|vmot|vm$");
  std::vector<std::pair<int, NetId>> mv;
  for (NetId n : rails)
    if (const auto v = net_millivolts(c.ix.net_name(n))) mv.emplace_back(*v, n);
  std::sort(mv.begin(), mv.end());
  if (mv.size() >= 2 && mv.front().first != mv.back().first) {
    if (!vin) vin = mv.back().second;
    if (!vout) vout = mv.front().second;
    return;
  }
  std::vector<NetId> ins, outs;
  for (NetId n : rails) (search(std::string(net_leaf(c.ix.net_name(n))), in_name) ? ins : outs).push_back(n);
  if (ins.size() == 1 && !outs.empty()) {
    if (!vin) vin = ins.front();
    if (!vout) vout = outs.front();
  }
}

Bound bind_regulator(const Ctx& c, int anchor, bool switching) {
  Bound r;
  if (c.ix.pad_count(anchor) < 3) {
    r.ok = false;
    r.why = "fewer than three pads";
    return r;
  }
  NetId vin = 0, vout = 0, sw = 0, fb = 0;
  regulator_rails(c, anchor, vin, vout, sw, fb);
  Role ic = make_role("ic");
  ic.parts.push_back(anchor);
  r.roles.push_back(ic);
  Role inductor = make_role("inductor"), swr = make_role("sw");
  if (switching) {
    // Inductor from an anchor signal pin (SW) to another net; the topology detector wants an output cap on it.
    for (NetId n : c.ix.fp_nets(anchor)) {
      if (c.ix.ground(n) || (sw && n != sw)) continue;
      for (int f : c.ix.parts_on(n)) {
        if (f == anchor || c.ix.letters(f) != "L" || c.b.footprints[static_cast<std::size_t>(f)].pads.size() != 2) continue;
        NetId other = 0;
        for (NetId m : c.ix.fp_nets(f))
          if (m != n) other = m;
        if (!other || !c.ix.signal(n)) continue;  // the SW node is neither a supply nor ground by name
        add_unique(inductor.parts, f);
        add_unique_net(swr.nets, n);
        if (!caps_to_ground(c, other).empty() &&
            std::find(r.topology.begin(), r.topology.end(), "inductor_from_sw_pin_to_output_cap") == r.topology.end())
          r.topology.push_back("inductor_from_sw_pin_to_output_cap");
        if (!vout && c.ix.supply(other)) vout = other;  // buck: the inductor's far end is the output rail
      }
    }
  }
  Role vinr = make_role("vin"), voutr = make_role("vout"), cin = make_role("cin"), cout = make_role("cout");
  if (vin) {
    vinr.nets.push_back(vin);
    vinr.pads.push_back(c.ix.pad_on(anchor, vin));
    cin.parts = own_caps(c, vin, anchor);
  }
  if (vout) {
    voutr.nets.push_back(vout);
    if (const int p = c.ix.pad_on(anchor, vout); p >= 0) voutr.pads.push_back(p);
    cout.parts = own_caps(c, vout, anchor);
    if (cout.parts.empty() && !inductor.parts.empty()) cout.parts = own_caps(c, vout, inductor.parts.front());
  }
  for (Role* x : {&vinr, &voutr, &cin, &cout, &inductor, &swr})
    if (!x->empty()) r.roles.push_back(*x);
  if (fb) {
    Role fbr = make_role("fb");
    fbr.nets.push_back(fb);
    fbr.pads.push_back(c.ix.pad_on(anchor, fb));
    r.roles.push_back(fbr);
  }
  return r;
}

// ---- Protection, connectors ------------------------------------------------------------------------------------

Bound bind_esd(const Ctx& c, int anchor) {
  Bound r;
  const std::string& l = c.ix.letters(anchor);
  static const std::set<std::string> passive = {"R", "C", "L", "FB", "F", "J", "P", "CN", "SW", "Y", "X", "K", "BT", "H", "MH", "TP"};
  if (passive.count(l) || c.ix.is_connector(anchor)) {
    r.ok = false;
    r.why = "reference " + l + " is not a protection part";
    return r;
  }
  Role tvs = make_role("tvs"), gnd = make_role("gnd"), prot = make_role("protected"), conn = make_role("connector"), ic = make_role("ic");
  tvs.parts.push_back(anchor);
  std::vector<NetId> nets;
  for (int pi : c.b.footprints[static_cast<std::size_t>(anchor)].pads) {
    const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
    if (p.net <= 0) continue;
    if (c.ix.ground(p.net)) gnd.pads.push_back(pi), add_unique_net(gnd.nets, p.net);
    else add_unique_net(nets, p.net);
  }
  std::sort(nets.begin(), nets.end());
  // Connector sharing the most protected nets; ties: natural reference order.
  int best = -1, best_n = 0;
  for (std::size_t f = 0; f < c.b.footprints.size(); ++f) {
    const int fi = static_cast<int>(f);
    if (fi == anchor || !c.ix.is_connector(fi)) continue;
    int k = 0;
    for (NetId n : nets) k += std::binary_search(c.ix.fp_nets(fi).begin(), c.ix.fp_nets(fi).end(), n) ? 1 : 0;
    if (k > best_n || (k == best_n && k > 0 && natural_less(ref(c, fi), ref(c, best)))) best = fi, best_n = k;
  }
  if (best >= 0) {
    conn.parts.push_back(best);
    for (NetId n : nets)
      if (std::binary_search(c.ix.fp_nets(best).begin(), c.ix.fp_nets(best).end(), n)) prot.nets.push_back(n);
  } else {
    prot.nets = nets;
  }
  if (!gnd.pads.empty() && best >= 0) r.topology.push_back("diode_array_signal_to_ground_near_connector_net");
  const std::vector<int> skip = {anchor};
  for (NetId n : prot.nets)
    for (int f : ics_near(c, n, skip)) add_unique(ic.parts, f);
  std::sort(ic.parts.begin(), ic.parts.end());
  r.roles.push_back(tvs);
  for (Role* x : {&conn, &prot, &ic, &gnd})
    if (!x->empty()) r.roles.push_back(*x);
  return r;
}

Bound bind_connector(const Ctx& c, int anchor) {
  Bound r;
  // Parts, jumpers and test points: not external connectors even on a header footprint.
  static const std::set<std::string> not_conn = {"U", "IC", "R", "C", "L", "D", "Q", "Y", "SW", "S", "F", "FB", "TP", "H", "MH", "LED", "BT", "RV", "T", "Z", "ZD", "VR", "RN", "M", "JP", "SJ", "W", "NT", "TEST", "FID"};
  if (not_conn.count(c.ix.letters(anchor)) || c.ix.pad_count(anchor) == 0) {
    r.ok = false;
    r.why = "reference " + c.ix.letters(anchor) + " is not a connector";
    return r;
  }
  Role conn = make_role("connector");
  conn.parts.push_back(anchor);
  r.roles.push_back(conn);
  return r;
}

// CAN / RS-485 transceivers: bus nets, the bus connector, protection and termination on the bus.
Bound bind_bus_xcvr(const Ctx& c, int anchor, const std::regex& pin_re, const std::regex& net_re) {
  Bound r;
  Role x = make_role("xcvr"), bus = make_role("bus"), conn = make_role("connector"), esd = make_role("esd"), term = make_role("term");
  x.parts.push_back(anchor);
  for (int pi : c.b.footprints[static_cast<std::size_t>(anchor)].pads) {
    const auto& p = c.b.pads[static_cast<std::size_t>(pi)];
    if (p.net > 0 && (search(p.pinfunction, pin_re) || search(std::string(net_leaf(c.ix.net_name(p.net))), net_re))) add_unique_net(bus.nets, p.net);
  }
  std::sort(bus.nets.begin(), bus.nets.end());
  r.roles.push_back(x);
  if (bus.nets.empty()) return r;
  r.roles.push_back(bus);
  std::vector<int> parts;
  for (NetId n : bus.nets)
    for (int f : c.ix.parts_on(n)) add_unique(parts, f);
  for (NetId n : bus.nets)
    for (NetId m : across_series(c, n, {anchor}))
      for (int f : c.ix.parts_on(m)) add_unique(parts, f);
  std::sort(parts.begin(), parts.end());
  for (int f : parts) {
    if (f == anchor) continue;
    if (c.ix.is_connector(f)) {
      if (conn.parts.empty()) conn.parts.push_back(f);
      continue;
    }
    const std::string& l = c.ix.letters(f);
    bool on_gnd = false;
    int on_bus = 0;
    for (NetId n : c.ix.fp_nets(f)) {
      on_gnd |= c.ix.ground(n);
      on_bus += std::binary_search(bus.nets.begin(), bus.nets.end(), n) ? 1 : 0;
    }
    if (l == "R" && on_bus == 2) term.parts.push_back(f);
    else if (on_gnd && on_bus >= 1 && l != "R" && l != "C" && !c.ix.is_ic(f)) esd.parts.push_back(f);
  }
  for (Role* y : {&conn, &esd, &term})
    if (!y->empty()) r.roles.push_back(*y);
  return r;
}

Bound bind_module(const Ctx& c, int anchor, const char* anchor_role) {
  Bound r;
  Role m = make_role(anchor_role);
  m.parts.push_back(anchor);
  r.roles.push_back(m);
  // The antenna region is the module footprint's own keep-out (rule area), when the library footprint has one.
  Role region = make_role("antenna_region");
  for (std::size_t zi = 0; zi < c.b.zones.size(); ++zi)
    if (c.b.zones[zi].footprint == anchor && c.b.zones[zi].rule_area) region.parts.push_back(anchor), region.pads.push_back(static_cast<int>(zi));
  if (!region.pads.empty()) {
    region.parts.resize(1);
    r.roles.push_back(region);  // `pads` holds zone indices for this role
  }
  return r;
}

Bound bind_default(const Ctx& c, int anchor, const Category& cat) {
  (void)c;
  Bound r;
  // The catalogue names the anchor role "anchor" in its roles map.
  std::string name = "anchor";
  for (const auto& [k, v] : cat.roles)
    if (v == "anchor") {
      name = k;
      break;
    }
  Role a = make_role(name);
  a.parts.push_back(anchor);
  r.roles.push_back(a);
  return r;
}

// Ethernet (doc 15 §8.1 `ethernet`): the anchor is the RJ45 (a connector) or the PHY (an IC); `magnetics` are the
// discrete LAN transformers that share at least two signal nets with the anchor (the line pairs of the RJ45, or the
// MDI pairs of the PHY) and have at least six pads (a two-pair 1:1 transformer; four-pad common-mode chokes are not
// magnetics). An RJ45 with integrated magnetics (MagJack) needs no void (ETH-05 note): role `integrated_magnetics`.
Bound bind_ethernet(const Ctx& c, int anchor) {
  Bound r;
  const auto& fp = c.b.footprints[static_cast<std::size_t>(anchor)];
  const bool phy_anchor = c.ix.is_ic(anchor) && !c.ix.is_connector(anchor);
  Role a = make_role(phy_anchor ? "phy" : "rj45");
  a.parts.push_back(anchor);
  r.roles.push_back(a);
  static const std::regex integrated = icase(R"(magjack|hr911|hr961|hy911|j00\d\d|arjm|arjc|lpj|lmj|hfj1|pulsetrans|trafo|with.?magnetics|integrated.?magnetics|_mag(_|$))");
  if (!phy_anchor && (search(fp.lib_id, integrated) || search(fp.value, integrated))) {
    Role im = make_role("integrated_magnetics");
    im.parts.push_back(anchor);
    r.roles.push_back(im);
    return r;
  }
  static const std::regex transformer =
      icase(R"(transformer|magnetics|lan.?trans|h1102|h1601|h5007|hx1188|hx1198|hx5004|hx5008|tg110|tg111|749010|7490100|s558-|hr6010|ns892|g2406|13f-)");
  std::vector<int> mags;
  for (NetId n : c.ix.fp_nets(anchor)) {
    if (!c.ix.signal(n)) continue;
    for (int f : c.ix.parts_on(n)) {
      if (f == anchor || std::find(mags.begin(), mags.end(), f) != mags.end() || c.ix.pad_count(f) < 6) continue;
      const auto& t = c.b.footprints[static_cast<std::size_t>(f)];
      const std::string& l = c.ix.letters(f);
      if (!(l == "T" || l == "TR" || search(t.lib_id, transformer) || search(t.value, transformer))) continue;
      int shared = 0;
      for (NetId m : c.ix.fp_nets(f)) shared += c.ix.signal(m) && std::binary_search(c.ix.fp_nets(anchor).begin(), c.ix.fp_nets(anchor).end(), m) ? 1 : 0;
      if (shared >= 2) mags.push_back(f);
    }
  }
  std::sort(mags.begin(), mags.end());
  if (!mags.empty()) {
    Role m = make_role("magnetics");
    m.parts = mags;
    r.roles.push_back(m);
  }
  return r;
}

Bound bind(const Ctx& c, const Category& cat, int anchor) {
  const std::string& id = cat.id;
  if (id == "usb2") return bind_usb(c, anchor, false);
  if (id == "usb3_typec") return bind_usb(c, anchor, true);
  if (id == "crystal") return bind_crystal(c, anchor);
  if (id == "oscillator") return bind_oscillator(c, anchor);
  if (id == "ic_decoupling") return bind_decoupling(c, anchor);
  if (id == "ldo") return bind_regulator(c, anchor, false);
  if (id == "buck" || id == "boost") return bind_regulator(c, anchor, true);
  if (id == "esd_tvs") return bind_esd(c, anchor);
  if (id == "connector_general") return bind_connector(c, anchor);
  if (id == "ethernet") return bind_ethernet(c, anchor);
  if (id == "rf_module") return bind_module(c, anchor, "module");
  if (id == "chip_antenna") return bind_module(c, anchor, "antenna");
  if (id == "can") {
    static const std::regex p = icase("^can_?[hl]$"), n = icase(R"(can_?[hl]$|^can[hl]|^can[_-]?(hi|lo|high|low)$)");
    return bind_bus_xcvr(c, anchor, p, n);
  }
  if (id == "rs485") {
    static const std::regex p = icase("^(a|b|y|z)$"), n = icase(R"(rs485|485_?[ab+-]$|^(rs)?485)");
    return bind_bus_xcvr(c, anchor, p, n);
  }
  return bind_default(c, anchor, cat);
}

// Categories that cannot share an anchor (doc 15 §3.4): the higher confidence wins, equal -> catalogue order.
int exclusive_group(const std::string& id) {
  if (id == "buck" || id == "boost" || id == "ldo") return 1;
  if (id == "crystal" || id == "oscillator") return 2;
  if (id == "rf_module" || id == "chip_antenna") return 3;
  return 0;
}

// Categories whose binding works from net names and topology alone, so a board without pin names does not cap them.
bool binds_without_pin_names(const std::string& id) { return id == "ic_decoupling"; }

// Attaches the user's `disable` and `set` entries (doc 15 §6.3) to the detected instances: global entries
// first, then entries for one reference, each group in file order, so @REF wins and a later entry wins.
void apply_rule_overrides(const model::Board& b, const Catalogue& cat, const Overrides& ov, Detection& det) {
  using K = OverrideEntry::Kind;
  auto slot = [](Instance& in, const std::string& rule) -> RuleOverride& {
    for (auto& o : in.overrides)
      if (o.rule == rule) return o;
    in.overrides.push_back(RuleOverride{});
    in.overrides.back().rule = rule;
    return in.overrides.back();
  };
  for (const bool with_ref : {false, true})
    for (const auto& e : ov.entries) {
      if ((e.kind != K::Disable && e.kind != K::Set) || e.ref.empty() == with_ref) continue;
      const int ci = cat.index_of(e.category);
      int hits = 0;
      for (auto& in : det.instances) {
        if (in.category != ci || (!e.ref.empty() && b.footprints[static_cast<std::size_t>(in.anchor)].reference != e.ref)) continue;
        ++hits;
        for (const RuleSpec& r : cat.categories[static_cast<std::size_t>(ci)].rules) {
          if (!e.rule.empty() && r.id != e.rule) continue;
          RuleOverride& o = slot(in, r.id);
          if (e.kind == K::Disable) {
            if (!o.disabled) o.notes.push_back("disabled (" + e.text + ")");
            o.disabled = true;
            continue;
          }
          if (!o.has_params) o.params = r.params, o.has_params = true;
          o.params[e.param] = e.value;
          std::erase_if(o.notes, [&](const std::string& n) { return n.starts_with(e.param + " = "); });
          o.notes.push_back(e.param + " = " + e.value.dump() + " (catalogue " + r.params[e.param].dump() + "; " + e.text + ")");
        }
      }
      if (hits == 0)
        det.override_unused.push_back(e.text + ": " +
                                      (e.ref.empty() ? "no " + e.category + " instance on this board"
                                                     : e.ref + " is not detected as " + e.category + " (assert it to force the category)"));
    }
  for (const auto& e : ov.entries) {
    if (e.kind != K::Assert) continue;
    bool found = false;
    for (const auto& in : det.instances)
      found = found || (cat.categories[static_cast<std::size_t>(in.category)].id == e.category && b.footprints[static_cast<std::size_t>(in.anchor)].reference == e.ref);
    if (!found) det.override_unused.push_back(e.text + ": " + e.ref + " has no pads (nothing to bind)");
  }
}

}  // namespace

Detection detect(const model::Board& b, const Catalogue& cat, const Overrides* ov) {
  const BoardIndex ix(b);
  Ctx c{b, ix, {}};
  for (const auto& t : decap_ties(b, [](int) { return true; }, true)) c.decap.emplace(t.cap_fp, t);
  Detection det;
  det.board_has_pin_names = ix.has_pin_names();
  if (ov) {
    check_override_refs(*ov, b);
    det.override_source = ov->source;
    for (const auto& e : ov->entries) det.override_entries.push_back(e.text);
  }
  // `assert` / `deny` entries for (category, reference).
  auto user = [&](OverrideEntry::Kind k, const std::string& category, const std::string& ref) {
    if (!ov) return false;
    for (const auto& e : ov->entries)
      if (e.kind == k && e.category == category && e.ref == ref) return true;
    return false;
  };
  if (ov)  // Two asserted categories that exclude each other on one part (doc 15 §3.4): the user must pick one.
    for (const auto& a : ov->entries)
      for (const auto& x : ov->entries)
        if (&a < &x && a.kind == OverrideEntry::Kind::Assert && x.kind == OverrideEntry::Kind::Assert && a.ref == x.ref &&
            exclusive_group(a.category) != 0 && exclusive_group(a.category) == exclusive_group(x.category))
          throw std::runtime_error(ov->source + ": " + a.ref + " is asserted as both " + a.category + " and " + x.category +
                                   ", which exclude each other (doc 15 §3.4)");

  std::vector<Instance> all;
  for (std::size_t ci = 0; ci < cat.categories.size(); ++ci) {
    const Category& C = cat.categories[ci];
    const bool has_topology = C.has_signal(Signal::Topology);
    for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
      const int f = static_cast<int>(fi);
      const auto& fp = b.footprints[fi];
      if (fp.pads.empty()) continue;  // logos and drawings: nothing to bind (a mounting hole has an NPTH pad)
      const bool asserted = user(OverrideEntry::Kind::Assert, C.id, fp.reference);
      const bool denied = user(OverrideEntry::Kind::Deny, C.id, fp.reference);
      // Non-topology detectors on the anchor's own fields.
      std::vector<char> hit(C.detect.size(), 0);
      bool any = false;
      for (std::size_t di = 0; di < C.detect.size(); ++di) {
        const Detector& d = C.detect[di];
        bool h = false;
        switch (d.signal) {
          case Signal::RefPrefix: h = search(fp.reference, d.re); break;
          case Signal::LibId: {
            // A pattern written as "library:footprint" matches the whole lib_id; any other pattern matches the
            // footprint name only, because library nicknames are free text ("usb_ccb_custom:QFN-48" is not a USB
            // connector; PCBench UCCBPCB).
            const auto colon = fp.lib_id.rfind(':');
            const bool whole = d.pattern.find(':') != std::string::npos || colon == std::string::npos;
            h = search(whole ? fp.lib_id : fp.lib_id.substr(colon + 1), d.re);
            break;
          }
          case Signal::Value: h = search(fp.value, d.re); break;
          case Signal::Keywords: h = search(fp.description + " " + fp.keywords, d.re) && !(fp.description.empty() && fp.keywords.empty()); break;
          case Signal::NetName:
            for (NetId n : ix.fp_nets(f)) h = h || search(ix.net_name(n), d.re);
            break;
          case Signal::PinName:
          case Signal::Topology: break;  // after binding
        }
        hit[di] = h ? 1 : 0;
        // A reference prefix alone never starts a detection (it would list every J* and U*).
        any = any || (h && d.signal != Signal::RefPrefix);
      }
      // Pin names on the anchor (may also make the anchor a candidate).
      bool pin_hit_any = false;
      for (std::size_t di = 0; di < C.detect.size(); ++di) {
        if (C.detect[di].signal != Signal::PinName) continue;
        for (int pi : fp.pads)
          if (search(b.pads[static_cast<std::size_t>(pi)].pinfunction, C.detect[di].re)) hit[di] = 1;
        pin_hit_any = pin_hit_any || hit[di];
      }
      any = any || pin_hit_any;
      if (!any && !has_topology && !asserted) continue;
      Bound bd = bind(c, C, f);
      if (!bd.pin_pads.empty())
        for (std::size_t di = 0; di < C.detect.size(); ++di) {
          if (C.detect[di].signal != Signal::PinName) continue;
          hit[di] = 0;
          for (int pi : bd.pin_pads)
            if (search(b.pads[static_cast<std::size_t>(pi)].pinfunction, C.detect[di].re)) hit[di] = 1;
        }
      bool part_level = false;
      for (std::size_t di = 0; di < C.detect.size(); ++di) {
        const Signal s = C.detect[di].signal;
        part_level = part_level || (hit[di] && (s == Signal::LibId || s == Signal::Value || s == Signal::Keywords));
      }
      for (std::size_t di = 0; di < C.detect.size(); ++di)
        if (C.detect[di].signal == Signal::Topology)
          hit[di] = (!bd.topology_weak || part_level) &&
                    std::find(bd.topology.begin(), bd.topology.end(), C.detect[di].pattern) != bd.topology.end() ? 1 : 0;
      Instance in;
      in.category = static_cast<int>(ci);
      in.anchor = f;
      int sum = 0;
      bool strong = false;
      for (std::size_t di = 0; di < C.detect.size(); ++di) {
        if (!hit[di]) continue;
        sum += C.detect[di].weight;
        strong = strong || C.detect[di].signal != Signal::RefPrefix;
        in.evidence.push_back(std::string(signal_name(C.detect[di].signal)) + " +" + std::to_string(C.detect[di].weight));
      }
      if (!strong && !asserted) continue;
      in.confidence = std::min(sum, 100);
      if (!ix.has_pin_names() && C.has_signal(Signal::PinName) && !binds_without_pin_names(C.id) && in.confidence > kNoPinNameCap) {
        in.confidence = kNoPinNameCap;
        in.capped = true;
      }
      if (denied) {  // the user says this part is not this category: listed, never applied (doc 15 §3.5)
        in.superseded_by = "denied by user (" + ov->source + ")";
        det.possible.push_back(std::move(in));
        continue;
      }
      if (asserted) {  // the user says it is: full confidence, and the binder's objection is overruled
        in.asserted = true;
        in.confidence = 100;
        in.capped = false;
        in.evidence.push_back("user assert");
        if (!bd.ok) {
          in.evidence.push_back("binder: " + bd.why);
          bd = bind_default(c, f, C);
        }
      }
      if (!bd.ok) {
        in.superseded_by = bd.why;
        if (in.confidence >= kPossibleMin) det.possible.push_back(std::move(in));
        continue;
      }
      in.roles = std::move(bd.roles);
      all.push_back(std::move(in));
    }
  }
  // Conflicts on one anchor within an exclusive group.
  for (std::size_t i = 0; i < all.size(); ++i) {
    const int g = exclusive_group(cat.categories[static_cast<std::size_t>(all[i].category)].id);
    if (g == 0 || all[i].confidence < cat.suggest) continue;
    for (std::size_t j = 0; j < all.size(); ++j) {
      if (j == i || all[j].anchor != all[i].anchor || all[j].confidence < cat.suggest) continue;
      if (exclusive_group(cat.categories[static_cast<std::size_t>(all[j].category)].id) != g) continue;
      if (all[i].asserted) break;  // the user's choice wins (two asserted members of one group are refused above)
      const bool j_wins = all[j].asserted || all[j].confidence > all[i].confidence ||
                          (all[j].confidence == all[i].confidence && all[j].category < all[i].category);
      if (j_wins) {
        all[i].superseded_by = cat.categories[static_cast<std::size_t>(all[j].category)].id + (all[j].asserted ? " (asserted by user)" : "");
        break;
      }
    }
  }
  for (auto& in : all) {
    if (in.confidence >= cat.suggest && in.superseded_by.empty()) det.instances.push_back(std::move(in));
    else if (in.confidence >= kPossibleMin) det.possible.push_back(std::move(in));
  }
  auto order = [&](const Instance& x, const Instance& y) {
    if (x.category != y.category) return x.category < y.category;
    const std::string &rx = b.footprints[static_cast<std::size_t>(x.anchor)].reference, &ry = b.footprints[static_cast<std::size_t>(y.anchor)].reference;
    if (rx != ry) return natural_less(rx, ry);
    return x.anchor < y.anchor;
  };
  std::sort(det.instances.begin(), det.instances.end(), order);
  std::sort(det.possible.begin(), det.possible.end(), order);
  if (ov) apply_rule_overrides(b, cat, *ov, det);
  return det;
}

}  // namespace tmk::crules
