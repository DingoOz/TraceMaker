// SPDX-License-Identifier: GPL-3.0-or-later
// Impedance and current rules on a board (doc 15 §5.3-5.4, P4). Report-only.
#include "crules/impedance_rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace tmk::crules {

namespace {

constexpr Coord kWidthFloor = 100'000;     // 0.1 mm: commodity fab minimum when the board sets none
constexpr Coord kGapDefault = 200'000;     // 0.2 mm: KiCad's default clearance when no project rules were read
constexpr Coord kMaxPairWidth = 5'000'000;
constexpr Coord kMaxWidth = 10'000'000;
constexpr Coord kMaxGap = 5'000'000;
constexpr Coord kOneOz = 35'000;           // 1 oz/ft^2 copper, nm
constexpr double kDefaultDeltaT = 10.0;    // PWR-01 default temperature rise, C
constexpr Coord kWideDiffWarn = 1'000'000; // wider than this: plain coupled microstrip is impractical (doc 15 §5.3)

std::string mm3(Coord v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", nm_to_mm(v));
  return buf;
}
std::string fmt(int decimals, double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

// What the zones on a reference layer say about it being a plane: a zone of a ground net confirms it ("GND zone");
// other zones or none leave the plane assumed (dev/assumptions-m13.md, P4).
bool ground_name(const std::string& n) {
  std::string u;
  for (char c : n) u += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  return u.find("GND") != std::string::npos || u.find("VSS") != std::string::npos || u.find("GROUND") != std::string::npos;
}
std::string zone_note(const model::Board& b, int copper) {
  std::set<std::string> grounds;
  bool other = false;
  for (const auto& z : b.zones) {
    if (z.rule_area || z.net == 0 || !(z.copper & model::layer_bit(copper))) continue;
    const std::string& n = b.nets[static_cast<std::size_t>(z.net)].name;
    if (ground_name(n)) grounds.insert(n);
    else other = true;
  }
  (void)other;  // signal pours do not make a plane
  if (grounds.empty()) return "assumed";
  std::string s;
  for (const auto& n : grounds) s += (s.empty() ? "" : ",") + n;
  return s;
}

double param(const RuleSpec& r, const char* k) {
  return r.params.contains(k) && r.params[k].is_number() ? r.params[k].get<double>() : 0.0;
}

// Impedance of a structure on a layer for a width (and gap where the structure has one).
double z_single(const LayerGeometry& g, imp::Structure s, Coord w, Coord gap, double* eps_eff) {
  imp::Line l;
  if (s == imp::Structure::Stripline) l = imp::stripline(w, g.h1, g.h2, g.t, g.er);
  else if (s == imp::Structure::Gcpw) l = imp::gcpw(w, gap, g.h1, g.t, g.er);
  else l = imp::microstrip(w, g.h1, g.t, g.er);
  if (eps_eff) *eps_eff = l.eps_eff;
  return l.z0;
}
double z_diff(const LayerGeometry& g, imp::Structure s, Coord w, Coord gap, double* eps_eff) {
  const imp::Pair p = s == imp::Structure::Stripline ? imp::coupled_stripline(w, gap, g.h1, g.h2, g.t, g.er)
                                                     : imp::coupled_microstrip(w, gap, g.h1, g.t, g.er);
  if (eps_eff) *eps_eff = p.eps_eff_odd;
  return p.zdiff();
}

std::string ref_text(const model::Board& b, const LayerGeometry& g) {
  std::string r = b.copper_name(g.ref_a) + " (" + g.ref_a_note + ")";
  if (g.structure == imp::Structure::Stripline) r += " and " + b.copper_name(g.ref_b) + " (" + g.ref_b_note + ")";
  return r;
}

LineSolution solve_line(const model::Board& b, const LayerGeometry& g, imp::Structure s, bool diff, double target, const ImpedancePlan& plan) {
  LineSolution L;
  L.layer = g.layer;
  L.ref = ref_text(b, g);
  L.structure = s;
  L.differential = diff;
  L.target = target;
  L.h = g.h1;
  L.h2 = s == imp::Structure::Stripline ? g.h2 : 0;
  L.er = g.er;
  const bool asym = s == imp::Structure::Stripline && g.h1 != g.h2;
  L.error_pct = imp::formula_error_pct(s, diff, asym);
  imp::Solved sv;
  if (diff) {
    L.gap = plan.pair_gap;
    sv = imp::solve_monotone([&](Coord w) { return z_diff(g, s, w, L.gap, nullptr); }, target, plan.min_width, kMaxPairWidth, true);
    if (!sv.ok && sv.value == plan.min_width) {
      // Even the narrowest allowed width is below the target at this gap: keep the width, widen the gap.
      L.width = plan.min_width;
      sv = imp::solve_monotone([&](Coord gap) { return z_diff(g, s, L.width, gap, nullptr); }, target, plan.pair_gap, kMaxGap, false);
      if (sv.ok) {
        L.gap = sv.value;
        L.note = "gap widened: the width is at the board minimum";
      }
    } else {
      L.width = sv.value;
    }
    if (sv.ok) L.z = z_diff(g, s, L.width, L.gap, &L.eps_eff);
  } else {
    L.gap = s == imp::Structure::Gcpw ? plan.coplanar_gap : 0;
    sv = imp::solve_monotone([&](Coord w) { return z_single(g, s, w, L.gap, nullptr); }, target, plan.min_width, kMaxWidth, true);
    L.width = sv.value;
    if (sv.ok) L.z = z_single(g, s, L.width, L.gap, &L.eps_eff);
  }
  L.ok = sv.ok;
  if (!sv.ok) L.why = "not reachable: " + sv.why;
  if (L.ok) L.ps_per_mm = imp::prop_delay_ps_per_mm(L.eps_eff);
  if (L.ok && diff && s == imp::Structure::Microstrip && L.width > kWideDiffWarn)
    L.note = "wider than 1 mm: plain coupled microstrip is impractical on this dielectric; prefer coplanar with ground or short, "
             "matched, coupled routing (doc 15 §5.3)";
  return L;
}

}  // namespace

std::vector<LayerGeometry> stackup_geometry(const model::Board& b) {
  std::vector<LayerGeometry> out;
  const int n = b.copper_count();
  const model::Stackup& st = b.stackup;
  for (int i = 0; i < n; ++i) {
    LayerGeometry g;
    g.copper = i;
    g.layer = b.copper_name(i);
    g.routing = b.layers[static_cast<std::size_t>(b.copper[static_cast<std::size_t>(i)])].type != "power";
    const bool outer = i == 0 || i == n - 1;
    g.structure = outer ? imp::Structure::Microstrip : imp::Structure::Stripline;
    out.push_back(g);
    LayerGeometry& G = out.back();
    if (!st.present) {
      G.why = "no stackup in the board";
      continue;
    }
    if (n < 2) {
      G.why = "a single copper layer has no reference plane";
      continue;
    }
    G.t = st.copper_thickness(i);
    if (G.t <= 0) {
      G.why = "the stackup gives no copper thickness for " + G.layer;
      continue;
    }
    if (outer) {
      G.ref_a = i == 0 ? 1 : n - 2;
      const model::DielectricGap d = st.between(std::min(i, G.ref_a), std::max(i, G.ref_a));
      if (!d.complete) {
        G.why = "the stackup lacks the thickness or εr between " + G.layer + " and " + b.copper_name(G.ref_a);
        continue;
      }
      G.h1 = d.thickness;
      G.er = d.epsilon_r;
      G.ref_a_note = zone_note(b, G.ref_a);
    } else {
      G.ref_a = i - 1;
      G.ref_b = i + 1;
      const model::DielectricGap up = st.between(i - 1, i), dn = st.between(i, i + 1);
      if (!up.complete || !dn.complete) {
        G.why = "the stackup lacks the thickness or εr around " + G.layer;
        continue;
      }
      G.h1 = up.thickness;
      G.h2 = dn.thickness;
      // Both sides as one dielectric: series combination by thickness (as for sublayers, stackup.hpp).
      G.er = static_cast<double>(up.thickness + dn.thickness) /
             (static_cast<double>(up.thickness) / up.epsilon_r + static_cast<double>(dn.thickness) / dn.epsilon_r);
      G.ref_a_note = zone_note(b, G.ref_a);
      G.ref_b_note = zone_note(b, G.ref_b);
    }
    G.ok = true;
  }
  return out;
}

ImpedancePlan plan_impedance(const model::Board& b, const model::DesignRules* rules, const RuleSpec& r) {
  ImpedancePlan p;
  p.z0 = param(r, "z0_ohm");
  p.zdiff = param(r, "zdiff_ohm");
  p.tol_pct = param(r, "tol_pct");
  if (r.params.contains("structure") && r.params["structure"].is_string()) p.structure = r.params["structure"].get<std::string>();
  if (!b.stackup.present) {
    p.why = "not applied: no stackup in the board (doc 15 §3.6)";
    return p;
  }
  if (p.z0 <= 0 && p.zdiff <= 0) {
    p.why = "the rule names no impedance target";
    return p;
  }
  // Bounds: the narrowest width the board allows (never below 0.1 mm), the pair gap from the Default net class
  // (diff-pair gap, never below its clearance or the board minimum) and the coplanar gap = clearance.
  if (rules) {
    const model::NetClass& dc = rules->default_class();
    p.min_width = std::max({kWidthFloor, rules->minimums.track_width, Coord{0}});
    p.pair_gap = std::max({dc.diff_pair_gap, dc.clearance, rules->minimums.clearance});
    p.coplanar_gap = std::max(dc.clearance, rules->minimums.clearance);
    p.bounds_note = "width >= " + mm3(p.min_width) + " mm; pair gap " + mm3(p.pair_gap) + " mm from the Default net class";
  } else {
    p.min_width = kWidthFloor;
    p.pair_gap = p.coplanar_gap = kGapDefault;
    p.bounds_note = "no project rules read: width >= 0.100 mm, gap 0.200 mm";
  }
  const auto geo = stackup_geometry(b);
  bool any_inner = false;
  for (const auto& g : geo) any_inner = any_inner || (g.routing && g.structure == imp::Structure::Stripline);
  if (p.structure == "stripline" && !any_inner) {
    p.why = "not applied: the rule asks for stripline and the board has no inner routing layer";
    return p;
  }
  for (const auto& g : geo) {
    if (!g.routing) continue;
    imp::Structure s = g.structure;
    if (p.structure == "stripline" && s != imp::Structure::Stripline) continue;
    if (p.structure == "gcpw" && s == imp::Structure::Microstrip) s = imp::Structure::Gcpw;
    if (!g.ok) {
      LineSolution L;
      L.layer = g.layer;
      L.structure = s;
      L.why = g.why;
      if (p.zdiff > 0) {
        L.differential = true;
        L.target = p.zdiff;
        p.lines.push_back(L);
      }
      if (p.z0 > 0) {
        L.differential = false;
        L.target = p.z0;
        p.lines.push_back(L);
      }
      continue;
    }
    // A coplanar pair has no model here: pairs on GCPW layers use coupled microstrip.
    if (p.zdiff > 0) p.lines.push_back(solve_line(b, g, s == imp::Structure::Gcpw ? imp::Structure::Microstrip : s, true, p.zdiff, p));
    if (p.z0 > 0) p.lines.push_back(solve_line(b, g, s, false, p.z0, p));
  }
  p.computed = true;
  return p;
}

std::string impedance_text(const ImpedancePlan& p) {
  if (!p.computed) return p.why;
  // Layers with the same result are listed together (e.g. both outer layers of a symmetric stackup).
  std::string s = "report only, not applied to routing:";
  std::vector<bool> done(p.lines.size(), false);
  for (std::size_t i = 0; i < p.lines.size(); ++i) {
    if (done[i]) continue;
    const LineSolution& L = p.lines[i];
    auto key = [](const LineSolution& x) {
      return std::string(x.differential ? "d" : "s") + std::to_string(x.target) + imp::structure_name(x.structure) + (x.ok ? "1" : "0") + x.why +
             std::to_string(x.width) + "/" + std::to_string(x.gap) + "/" + std::to_string(std::min(x.h, x.h2 ? x.h2 : x.h)) + "/" +
             std::to_string(std::max(x.h, x.h2)) + "/" + fmt(4, x.er);
    };
    std::string layers, refs;
    for (std::size_t j = i; j < p.lines.size(); ++j)
      if (!done[j] && key(p.lines[j]) == key(L)) {
        done[j] = true;
        layers += (layers.empty() ? "" : ", ") + p.lines[j].layer;
        if (p.lines[j].ok) refs += (refs.empty() ? "" : "; ") + p.lines[j].layer + " over " + p.lines[j].ref;
      }
    s += "\n         " + fmt(0, L.target) + " Ω " + (L.differential ? "diff" : "SE") + " on " + layers + ": ";
    if (!L.ok) {
      s += L.why;
      continue;
    }
    s += mm3(L.width) + " mm";
    if (L.differential) s += " / gap " + mm3(L.gap) + " mm";
    else if (L.structure == imp::Structure::Gcpw) s += " / ground gap " + mm3(L.gap) + " mm";
    s += std::string(" ") + imp::structure_name(L.structure) + " (h " + mm3(L.h) + (L.h2 ? "/" + mm3(L.h2) : std::string()) + " mm, εr " +
         fmt(2, L.er) + ") = " + fmt(1, L.z) + " Ω, " + fmt(2, L.ps_per_mm) + " ps/mm, formula error ±" + fmt(0, L.error_pct) + " %";
    if (!L.note.empty()) s += "; " + L.note;
    s += "\n           reference: " + refs;
  }
  s += "\n         (reference (GND): a ground zone is on that layer; (assumed): no ground zone, plane assumed; " + p.bounds_note + "; solder mask ignored; fab tolerance ±10 % typical: ask the fab for controlled impedance)";
  return s;
}

double pair_impedance(const model::Board& b, int copper, Coord width, Coord gap) {
  const auto geo = stackup_geometry(b);
  if (copper < 0 || copper >= static_cast<int>(geo.size()) || !geo[static_cast<std::size_t>(copper)].ok || width <= 0 || gap <= 0) return -1;
  const LayerGeometry& g = geo[static_cast<std::size_t>(copper)];
  return z_diff(g, g.structure, width, gap, nullptr);
}

nlohmann::json impedance_json(const ImpedancePlan& p) {
  using nlohmann::json;
  json j = {{"computed", p.computed}};
  if (!p.computed) {
    j["why"] = p.why;
    return j;
  }
  if (p.z0 > 0) j["z0_ohm"] = p.z0;
  if (p.zdiff > 0) j["zdiff_ohm"] = p.zdiff;
  j["tol_pct"] = p.tol_pct;
  j["min_width_mm"] = nm_to_mm(p.min_width);
  j["pair_gap_mm"] = nm_to_mm(p.pair_gap);
  j["coplanar_gap_mm"] = nm_to_mm(p.coplanar_gap);
  j["bounds"] = p.bounds_note;
  json lines = json::array();
  for (const auto& L : p.lines) {
    json x = {{"layer", L.layer}, {"structure", imp::structure_name(L.structure)}, {"differential", L.differential}, {"target_ohm", L.target}, {"ok", L.ok}};
    if (!L.ok) {
      x["why"] = L.why;
    } else {
      x["reference"] = L.ref;
      x["width_mm"] = nm_to_mm(L.width);
      if (L.gap) x["gap_mm"] = nm_to_mm(L.gap);
      x["impedance_ohm"] = L.z;
      x["eps_eff"] = L.eps_eff;
      x["ps_per_mm"] = L.ps_per_mm;
      x["h_mm"] = nm_to_mm(L.h);
      if (L.h2) x["h2_mm"] = nm_to_mm(L.h2);
      x["epsilon_r"] = L.er;
      x["formula_error_pct"] = L.error_pct;
      if (!L.note.empty()) x["note"] = L.note;
    }
    lines.push_back(std::move(x));
  }
  j["lines"] = lines;
  return j;
}

CurrentPlan plan_current(const model::Board& b, const RuleSpec& r) {
  CurrentPlan p;
  p.amps = param(r, "current_a");
  p.delta_t_c = param(r, "delta_t_c");
  if (p.delta_t_c <= 0) p.delta_t_c = kDefaultDeltaT;
  if (p.amps <= 0) {
    p.why = "no current given (user override or net-name convention needed, doc 15 §3.6)";
    return p;
  }
  const int n = b.copper_count();
  if (b.stackup.present && b.stackup.copper_thickness(0) > 0) {
    p.outer_copper = b.stackup.copper_thickness(0);
    p.inner_copper = n > 2 ? b.stackup.copper_thickness(1) : 0;
    if (n > 2 && p.inner_copper <= 0) p.inner_copper = kOneOz;
  } else {
    p.copper_assumed = true;
    p.outer_copper = kOneOz;
    p.inner_copper = n > 2 ? kOneOz : 0;
  }
  p.outer_width = imp::width_for_current(p.amps, p.delta_t_c, p.outer_copper, true);
  if (p.inner_copper > 0) p.inner_width = imp::width_for_current(p.amps, p.delta_t_c, p.inner_copper, false);
  p.computed = true;
  return p;
}

std::string current_text(const CurrentPlan& p) {
  if (!p.computed) return p.why;
  std::string s = "report only: IPC-2221 width for " + fmt(1, p.amps) + " A at ΔT " + fmt(0, p.delta_t_c) + " °C: outer " +
                  mm3(p.outer_width) + " mm (" + fmt(0, nm_to_mm(p.outer_copper) * 1000.0) + " µm Cu)";
  if (p.inner_width) s += ", inner " + mm3(p.inner_width) + " mm (" + fmt(0, nm_to_mm(p.inner_copper) * 1000.0) + " µm Cu)";
  if (p.copper_assumed) s += "; copper thickness assumed 1 oz (no stackup)";
  return s;
}

nlohmann::json current_json(const CurrentPlan& p) {
  nlohmann::json j = {{"computed", p.computed}};
  if (!p.computed) {
    j["why"] = p.why;
    return j;
  }
  j["current_a"] = p.amps;
  j["delta_t_c"] = p.delta_t_c;
  j["formula"] = "IPC-2221B: I = k dT^0.44 A^0.725 (k 0.048 outer, 0.024 inner; A in mil^2)";
  j["outer_copper_mm"] = nm_to_mm(p.outer_copper);
  j["outer_width_mm"] = nm_to_mm(p.outer_width);
  if (p.inner_width) {
    j["inner_copper_mm"] = nm_to_mm(p.inner_copper);
    j["inner_width_mm"] = nm_to_mm(p.inner_width);
  }
  j["copper_assumed"] = p.copper_assumed;
  return j;
}

std::vector<LayerDelay> prop_delays(const model::Board& b, const Catalogue& cat) {
  std::vector<LayerDelay> out;
  const auto geo = stackup_geometry(b);
  for (const auto& g : geo) {
    LayerDelay d;
    d.layer = g.layer;
    const bool outer = g.structure == imp::Structure::Microstrip;
    d.ps_per_mm = outer ? cat.prop_delay_outer : cat.prop_delay_inner;
    if (g.ok) {
      double ee = g.er;
      if (outer) {
        // eps_eff of a microstrip depends a little on the width: take the 50 ohm line.
        const auto sv = imp::solve_monotone([&](Coord w) { return imp::microstrip(w, g.h1, g.t, g.er).z0; }, 50.0, kWidthFloor, kMaxWidth, true);
        ee = imp::microstrip(sv.value, g.h1, g.t, g.er).eps_eff;
      }
      d.ps_per_mm = imp::prop_delay_ps_per_mm(ee);
      d.from_stackup = true;
    }
    out.push_back(d);
  }
  return out;
}

std::string stackup_summary(const model::Board& b, const Catalogue& cat) {
  if (!b.stackup.present)
    return "no stackup in the board: impedance rules not applied; skew budgets use " + fmt(1, cat.prop_delay_outer) + " ps/mm outer / " +
           fmt(1, cat.prop_delay_inner) + " ps/mm inner (doc 15 §4)";
  Coord total = 0;
  for (const auto& l : b.stackup.layers)
    if (!l.is_mask()) total += l.thickness;
  std::string s = "stackup: " + std::to_string(b.copper_count()) + " copper layers, " + fmt(2, nm_to_mm(total)) + " mm";
  if (!b.stackup.copper_finish.empty() && b.stackup.copper_finish != "None") s += ", " + b.stackup.copper_finish;
  s += "; prop delay (ps/mm, 50 Ω line):";
  for (const auto& d : prop_delays(b, cat)) s += " " + d.layer + " " + fmt(2, d.ps_per_mm) + (d.from_stackup ? "" : " (default)");
  return s;
}

nlohmann::json stackup_json(const model::Board& b, const Catalogue& cat) {
  using nlohmann::json;
  json j = {{"present", b.stackup.present}};
  if (b.stackup.present) {
    json layers = json::array();
    for (const auto& l : b.stackup.layers) {
      json x = {{"name", l.name}, {"type", l.type}, {"thickness_mm", nm_to_mm(l.thickness)}};
      if (l.epsilon_r > 0) x["epsilon_r"] = l.epsilon_r;
      if (l.loss_tangent > 0) x["loss_tangent"] = l.loss_tangent;
      if (!l.material.empty()) x["material"] = l.material;
      if (l.sublayers > 1) x["sublayers"] = l.sublayers;
      layers.push_back(std::move(x));
    }
    j["layers"] = layers;
    if (!b.stackup.copper_finish.empty()) j["copper_finish"] = b.stackup.copper_finish;
    json geo = json::array();
    for (const auto& g : stackup_geometry(b)) {
      json x = {{"layer", g.layer}, {"routing", g.routing}, {"structure", imp::structure_name(g.structure)}, {"ok", g.ok}};
      if (g.ok) {
        x["reference"] = ref_text(b, g);
        x["h_mm"] = nm_to_mm(g.h1);
        if (g.structure == imp::Structure::Stripline) x["h2_mm"] = nm_to_mm(g.h2);
        x["copper_mm"] = nm_to_mm(g.t);
        x["epsilon_r"] = g.er;
      } else {
        x["why"] = g.why;
      }
      geo.push_back(std::move(x));
    }
    j["lines"] = geo;
  }
  json delays = json::array();
  for (const auto& d : prop_delays(b, cat)) delays.push_back({{"layer", d.layer}, {"ps_per_mm", d.ps_per_mm}, {"from_stackup", d.from_stackup}});
  j["prop_delay"] = delays;
  return j;
}

}  // namespace tmk::crules
