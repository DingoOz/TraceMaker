// SPDX-License-Identifier: GPL-3.0-or-later
// Proximity rules bound to pad pairs (doc 15 §4 `proximity`, §5.2): which pin of which part should be near
// which other pin. Used for the measurement in the report and for the placer's pseudo-nets.
#include <algorithm>

#include "crules/engine.hpp"
#include "geom/shape.hpp"

namespace tmk::crules {

namespace {

using model::NetId;

geom::i128 dist2(Point a, Point c) {
  const geom::i128 dx = a.x - c.x, dy = a.y - c.y;
  return dx * dx + dy * dy;
}

// The pad of footprint `fp` on net `n` nearest to `at` (ties: lowest pad index), or -1.
int nearest_pad(const model::Board& b, int fp, NetId n, Point at) {
  int best = -1;
  geom::i128 bd = 0;
  for (int pi : b.footprints[static_cast<std::size_t>(fp)].pads) {
    const auto& p = b.pads[static_cast<std::size_t>(pi)];
    if (p.net != n || n <= 0) continue;
    const geom::i128 d = dist2(p.pos, at);
    if (best < 0 || d < bd) best = pi, bd = d;
  }
  return best;
}

int first_pad(const model::Board& b, int fp, NetId n) {
  for (int pi : b.footprints[static_cast<std::size_t>(fp)].pads)
    if (b.pads[static_cast<std::size_t>(pi)].net == n) return pi;
  return -1;
}

double param_mm(const nlohmann::ordered_json& params, const char* key, double dflt) {
  if (params.contains(key) && params[key].is_number()) return params[key].get<double>();
  return dflt;
}

// For every non-ground net that part `f` shares with connector `conn`: (pad of f, nearest connector pad).
void pairs_to_connector(const model::Board& b, const BoardIndex& ix, int f, int conn, std::vector<std::pair<int, int>>& out) {
  for (NetId n : ix.fp_nets(f)) {
    if (ix.ground(n) || !std::binary_search(ix.fp_nets(conn).begin(), ix.fp_nets(conn).end(), n)) continue;
    const int pa = first_pad(b, f, n);
    const int pb = nearest_pad(b, conn, n, b.pads[static_cast<std::size_t>(pa)].pos);
    if (pa >= 0 && pb >= 0) out.emplace_back(pa, pb);
  }
}

}  // namespace

std::vector<ProximityPairs> proximity_pairs(const model::Board& b, const Catalogue& cat, const Detection& det) {
  std::vector<ProximityPairs> out;
  const BoardIndex ix(b);
  for (std::size_t ii = 0; ii < det.instances.size(); ++ii) {
    const Instance& in = det.instances[ii];
    const Category& C = cat.categories[static_cast<std::size_t>(in.category)];
    const int anchor = in.anchor;
    auto pos = [&](int pad) { return b.pads[static_cast<std::size_t>(pad)].pos; };
    for (const RuleSpec& r : C.rules) {
      if (in.disabled(r.id)) continue;  // disabled by the user override file: no measurement, no pseudo-net
      ProximityPairs pp;
      pp.spec = &r;
      pp.instance = static_cast<int>(ii);
      pp.limit_mm = param_mm(rule_params(in, r), "max_mm", 0);
      auto need = [&](const char* role) -> const Role* {
        const Role* x = in.role(role);
        if ((!x || x->empty()) && pp.unbound.empty()) pp.unbound = std::string("role ") + role + " not bound";
        return x && !x->empty() ? x : nullptr;
      };
      const std::string& id = r.id;
      if (id == "XTAL-01") {
        const Role* xr = need("crystal");
        const Role* ic = need("ic");
        if (xr && ic) {
          std::vector<int> pins;
          for (const char* nm : {"xin", "xout"})
            if (const Role* p = in.role(nm)) pins.insert(pins.end(), p->pads.begin(), p->pads.end());
          for (NetId n : xr->nets) {
            const int cp = first_pad(b, anchor, n);
            if (cp < 0) continue;
            int tp = nearest_pad(b, ic->parts.front(), n, pos(cp));
            if (tp < 0) {  // across a series resistor: the nearest oscillator pin
              geom::i128 bd = 0;
              for (int p : pins)
                if (tp < 0 || dist2(pos(p), pos(cp)) < bd) tp = p, bd = dist2(pos(p), pos(cp));
            }
            if (tp >= 0) pp.pairs.emplace_back(cp, tp);
          }
        }
      } else if (id == "XTAL-02") {
        const Role* xr = need("crystal");
        const Role* caps = need("load_caps");
        if (xr && caps)
          for (int f : caps->parts)
            for (NetId n : xr->nets)
              if (const int cp = first_pad(b, f, n); cp >= 0) pp.pairs.emplace_back(cp, nearest_pad(b, anchor, n, pos(cp)));
      } else if (id == "OSC-01") {
        if (const Role* d = need("decap"))
          for (int f : d->parts)
            for (NetId n : ix.fp_nets(f))
              if (ix.supply(n))
                if (const int cp = first_pad(b, f, n); cp >= 0)
                  if (const int tp = nearest_pad(b, anchor, n, pos(cp)); tp >= 0) pp.pairs.emplace_back(cp, tp);
      } else if (id == "OSC-02") {
        const Role* out_r = need("out");
        const Role* load = need("load");
        if (out_r && load)
          for (NetId n : out_r->nets) {
            const int op = first_pad(b, anchor, n);
            if (op < 0) continue;
            int tp = -1;
            geom::i128 bd = 0;
            for (int f : load->parts)
              if (const int q = nearest_pad(b, f, n, pos(op)); q >= 0 && (tp < 0 || dist2(pos(q), pos(op)) < bd)) tp = q, bd = dist2(pos(q), pos(op));
            if (op >= 0 && tp >= 0) pp.pairs.emplace_back(op, tp);
          }
      } else if (id == "DEC-01" || id == "DEC-07") {
        const Role* cap = need("cap");
        const Role* pin = need("pin");
        if (id == "DEC-07" && !in.role("vref") && !in.role("vcap")) {
          pp.unbound = "role vref/vcap not bound (not a reference or VCAP pin)";
        } else if (cap && pin) {
          pp.pairs.emplace_back(cap->pads.front(), pin->pads.front());
        }
      } else if (id == "LDO-01" || id == "BUCK-01") {
        for (const char* side : id == "LDO-01" ? std::vector<const char*>{"cin", "cout"} : std::vector<const char*>{"cin"}) {
          const Role* caps = in.role(side);
          const Role* rail = in.role(std::string_view(side) == "cin" ? "vin" : "vout");
          if (!caps || !rail) continue;
          const NetId n = rail->nets.front();
          for (int f : caps->parts)
            if (const int cp = first_pad(b, f, n); cp >= 0)
              if (const int tp = nearest_pad(b, anchor, n, pos(cp)); tp >= 0) pp.pairs.emplace_back(cp, tp);
        }
        if (pp.pairs.empty() && pp.unbound.empty()) pp.unbound = id == "LDO-01" ? "roles cin/cout not bound" : "role cin not bound";
      } else if (id == "BUCK-02") {
        const Role* l = need("inductor");
        const Role* sw = need("sw");
        if (l && sw) {
          pp.limit_mm = param_mm(rule_params(in, r), "inductor_max_mm", 0);
          for (int f : l->parts)
            for (NetId n : sw->nets)
              if (const int lp = first_pad(b, f, n); lp >= 0)
                if (const int tp = nearest_pad(b, anchor, n, pos(lp)); tp >= 0) pp.pairs.emplace_back(lp, tp);
        }
      } else if (id == "ESD-01") {
        const Role* conn = need("connector");
        const Role* prot = need("protected");
        if (conn && prot) {
          pp.weight = 20;
          for (NetId n : prot->nets) {
            const int tp = first_pad(b, anchor, n);
            if (tp >= 0)
              if (const int cp = nearest_pad(b, conn->parts.front(), n, pos(tp)); cp >= 0) pp.pairs.emplace_back(tp, cp);
          }
        }
      } else if (id == "USB2-07" || id == "USBC-10" || id == "CAN-02" || id == "RS485-02") {
        const char* conn_role = (id == "CAN-02" || id == "RS485-02") ? "connector" : nullptr;
        const int conn = conn_role ? (in.role(conn_role) ? in.role(conn_role)->parts.front() : -1) : anchor;
        if (conn < 0) {
          pp.unbound = "role connector not bound";
        } else {
          pp.weight = 20;
          bool any_role = false;
          for (const auto& role : r.applies_to)
            if (const Role* x = in.role(role)) {
              any_role = true;
              for (int f : x->parts) pairs_to_connector(b, ix, f, conn, pp.pairs);
            }
          if (!any_role) pp.unbound = "role " + r.applies_to.front() + " not bound";
        }
      } else {
        continue;  // not a proximity rule TraceMaker binds to pads (yet)
      }
      if (!pp.pairs.empty()) pp.unbound.clear();
      // Pairs must be distinct parts; drop degenerate ones.
      std::erase_if(pp.pairs, [&](const std::pair<int, int>& q) {
        return q.first < 0 || q.second < 0 || b.pads[static_cast<std::size_t>(q.first)].footprint == b.pads[static_cast<std::size_t>(q.second)].footprint;
      });
      if (pp.pairs.empty() && pp.unbound.empty()) pp.unbound = "no pin pair found";
      out.push_back(std::move(pp));
    }
  }
  return out;
}

}  // namespace tmk::crules
