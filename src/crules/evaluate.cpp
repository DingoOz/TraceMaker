// Effective rules (doc 15 §3.4-3.6, §5.5): every catalogue rule of every detected instance gets a status and,
// where TraceMaker can measure it, a measured value.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "crules/engine.hpp"
#include "crules/names.hpp"
#include "geom/shape.hpp"

namespace tmk::crules {

Mode parse_mode(std::string_view s) {
  if (s == "off") return Mode::Off;
  if (s == "report") return Mode::Report;
  if (s == "soft") return Mode::Soft;
  if (s == "on") return Mode::On;
  throw std::invalid_argument("component rules mode must be off, report, soft or on (got '" + std::string(s) + "')");
}

const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Off: return "off";
    case Mode::Report: return "report";
    case Mode::Soft: return "soft";
    case Mode::On: return "on";
  }
  return "?";
}

const char* status_name(Status s) {
  switch (s) {
    case Status::Applied: return "applied";
    case Status::SatisfiedByBoard: return "satisfied-by-board";
    case Status::NotApplied: return "not-applied";
    case Status::Advisory: return "advisory";
  }
  return "?";
}

namespace {

using model::NetId;

// Board outline pieces as segments (board-level Edge.Cuts graphics; arcs and circles flattened to 5 um).
std::vector<std::pair<Point, Point>> edge_segments(const model::Board& b) {
  std::vector<std::pair<Point, Point>> segs;
  auto chain = [&](const std::vector<Point>& pts, bool close) {
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) segs.emplace_back(pts[i], pts[i + 1]);
    if (close && pts.size() > 2) segs.emplace_back(pts.back(), pts.front());
  };
  for (const auto& g : b.graphics) {
    if (g.layer != "Edge.Cuts" || g.footprint >= 0) continue;
    switch (g.kind) {
      case model::Graphic::Kind::Line: segs.emplace_back(g.a, g.b); break;
      case model::Graphic::Kind::Arc: chain(geom::arc_points(g.a, g.c, g.b), false); break;
      case model::Graphic::Kind::Circle: {
        const Coord r = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y)));
        chain(geom::circle_points(g.a, r), false);
        break;
      }
      case model::Graphic::Kind::Rect: chain({g.a, Point{g.b.x, g.a.y}, g.b, Point{g.a.x, g.b.y}}, true); break;
      default: chain(g.pts, g.kind == model::Graphic::Kind::Poly); break;
    }
  }
  return segs;
}

// Outline points of a footprint: its courtyard graphics, else its pad centres.
std::vector<Point> body_points(const model::Board& b, int fi) {
  std::vector<Point> pts;
  const auto& fp = b.footprints[static_cast<std::size_t>(fi)];
  for (int gi : fp.graphics) {
    const auto& g = b.graphics[static_cast<std::size_t>(gi)];
    if (g.layer != "F.CrtYd" && g.layer != "B.CrtYd") continue;
    pts.push_back(g.a);
    pts.push_back(g.b);
    pts.insert(pts.end(), g.pts.begin(), g.pts.end());
  }
  if (pts.empty())
    for (int pi : fp.pads) pts.push_back(b.pads[static_cast<std::size_t>(pi)].pos);
  return pts;
}

std::string fmt_mm3(Coord v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", nm_to_mm(v));
  return buf;
}

double pad_dist_mm(const model::Board& b, int pa, int pb) {
  const Point a = b.pads[static_cast<std::size_t>(pa)].pos, c = b.pads[static_cast<std::size_t>(pb)].pos;
  return std::hypot(nm_to_mm(a.x - c.x), nm_to_mm(a.y - c.y));
}

// Why a rule of this kind is not applied in this milestone (doc 15 §10 phases).
std::string not_built_reason(const RuleSpec& r) {
  const std::string& k = r.kind;
  if (k == "diff_pair") return "per-net differential-pair routing not enabled for these nets (doc 15 P3)";
  if (k == "length_match" || k == "max_length" || k == "max_stub" || k == "via_limit") return "routing-stage rule, not built yet (doc 15 P3)";
  if (k == "reference_plane" || k == "stitching") return "needs plane-coverage checks (doc 15 P4)";
  if (k == "loop" || k == "copper_area" || k == "thermal_vias" || k == "width_for_current" || k == "kelvin" || k == "connection" ||
      k == "guard" || k == "net_weight")
    return "power/thermal rule, not built yet (doc 15 P5)";
  if (k == "clearance" || k == "creepage") return "role-to-role clearance not built yet (doc 15 P6)";
  if (k == "order") return "routing order (chain topology) not built yet (doc 15 P3)";
  if (k == "orientation") return "rotation restriction not built yet";
  if (k == "keepout") return "keep-out shape not generated for this category yet";
  if (k == "proximity") return "role not bound to pins for this category yet";
  return "not built yet";
}

// A board net class with an explicit diff-pair gap on every net of the roles: the board's own rule wins (§3.5).
std::string board_pair_class(const model::Board& b, const model::DesignRules* rules, const Instance& in, const RuleSpec& r) {
  if (!rules) return {};
  std::vector<NetId> nets;
  for (const auto& role : r.applies_to)
    if (const Role* x = in.role(role)) nets.insert(nets.end(), x->nets.begin(), x->nets.end());
  if (nets.size() < 2) return {};
  std::string cls;
  for (NetId n : nets) {
    const auto& nc = rules->class_for(b.nets[static_cast<std::size_t>(n)].name);
    if (nc.name == "Default" || !nc.has_diff_pair_gap) return {};
    if (!cls.empty() && cls != nc.name) return {};
    cls = nc.name;
  }
  return cls;
}

}  // namespace

Evaluation evaluate(const model::Board& b, const model::DesignRules* rules, const Catalogue& cat, const Detection& det, Mode mode) {
  Evaluation ev;
  ev.mode = mode;
  if (mode == Mode::Off) return ev;
  const auto prox = proximity_pairs(b, cat, det);
  std::map<std::pair<int, std::string>, const ProximityPairs*> prox_of;
  for (const auto& p : prox) prox_of[{p.instance, p.spec->id}] = &p;
  std::vector<std::string> ko_missing;
  const auto kos = generate_keepouts(b, cat, det, &ko_missing);
  std::map<std::pair<int, std::string>, std::vector<const GeneratedKeepout*>> ko_of;
  for (const auto& k : kos) ko_of[{k.instance, k.rule}].push_back(&k);
  const auto edges = edge_segments(b);

  for (std::size_t ii = 0; ii < det.instances.size(); ++ii) {
    const Instance& in = det.instances[ii];
    const Category& C = cat.categories[static_cast<std::size_t>(in.category)];
    const bool apply_level = in.confidence >= cat.apply;
    for (const RuleSpec& r : C.rules) {
      EffectiveRule e;
      e.instance = static_cast<int>(ii);
      e.spec = &r;
      e.severity = r.severity;
      // Confidence below `apply` demotes hard to soft (§3.2); so does unverified evidence (§8: R rules are never hard).
      if (e.severity == Severity::Hard && (!apply_level || r.evidence == "R")) e.severity = Severity::Soft;
      const auto key = std::make_pair(static_cast<int>(ii), r.id);
      // User override file (doc 15 §3.5 level 1): a disabled rule is neither applied nor measured.
      const RuleOverride* user = in.override_for(r.id);
      if (user && user->disabled) {
        e.status = Status::NotApplied;
        e.detail = "overridden by user: " + user->notes.front();
        ev.rules.push_back(std::move(e));
        continue;
      }
      // Rules whose parameters the user changed are evaluated on a copy with the new parameters.
      RuleSpec user_spec;
      if (user && user->has_params) {
        user_spec = r;
        user_spec.params = user->params;
      }
      const RuleSpec& rp = user && user->has_params ? user_spec : r;

      // Measurement first: proximity pairs and board-edge distances are reported in every mode.
      bool measured_rule = false;
      if (const auto it = prox_of.find(key); it != prox_of.end()) {
        measured_rule = true;
        const ProximityPairs& pp = *it->second;
        if (!pp.pairs.empty()) {
          Measure m;
          for (const auto& [pa, pb] : pp.pairs) m.value_mm = std::max(m.value_mm, pad_dist_mm(b, pa, pb));
          m.limit_mm = pp.limit_mm;
          m.met = m.limit_mm <= 0 || m.value_mm <= m.limit_mm + 1e-9;
          m.what = "largest pad-centre distance over " + std::to_string(pp.pairs.size()) + " pin pair(s)";
          e.measure = m;
        }
      }
      if (r.kind == "edge" && !edges.empty()) {
        measured_rule = true;
        double best = 1e300;
        for (int f : {in.anchor})
          for (const Point& q : body_points(b, f))
            for (const auto& [a, c] : edges) best = std::min(best, static_cast<double>(geom::point_seg_dist(q, a, c)));  // a distance (nm), not squared
        Measure m;
        m.value_mm = nm_to_mm(geom::kiround(best));
        double lim = 0;
        for (const char* k : {"max_mm", "antenna_side_to_edge_mm"})
          if (rp.params.contains(k) && rp.params[k].is_number()) lim = rp.params[k].get<double>();
        m.limit_mm = lim;
        m.met = m.value_mm <= lim + 1e-9;
        m.what = "nearest courtyard vertex (or pad centre) to Edge.Cuts";
        e.measure = m;
      }

      if (e.severity == Severity::Advisory) {
        e.status = Status::Advisory;
        e.detail = r.text;
      } else if (const auto it = prox_of.find(key); it != prox_of.end() && r.enforced_in("place")) {
        const ProximityPairs& pp = *it->second;
        bool all_locked = !pp.pairs.empty();
        for (const auto& q : pp.pairs) all_locked = all_locked && b.footprints[static_cast<std::size_t>(b.pads[static_cast<std::size_t>(q.first)].footprint)].locked;
        if (!pp.unbound.empty()) e.detail = pp.unbound;
        else if (all_locked) e.detail = "the parts are locked (never moved)";
        else if (mode == Mode::Report) e.detail = "report mode (checked only)";
        else {
          e.status = Status::Applied;
          e.detail = "placement pseudo-net, weight " + std::to_string(pp.weight);
        }
      } else if (r.kind == "keepout") {
        const bool module_ko = r.id == "RFM-02" || r.id == "ANT-01";
        if (module_ko && in.role("antenna_region")) {
          e.status = Status::SatisfiedByBoard;
          e.detail = "the footprint's own keep-out area";
        } else if (module_ko) {
          e.detail = "antenna region unknown: the footprint has no keep-out and there is no datasheet table yet";
        } else if (const auto k = ko_of.find(key); k != ko_of.end()) {
          std::string layers;
          for (const auto* g : k->second)
            for (const auto& l : g->zone.layers) layers += (layers.empty() ? "" : "+") + l;
          if (!apply_level) e.detail = "confidence " + std::to_string(in.confidence) + " below the apply threshold " + std::to_string(cat.apply);
          else if (mode == Mode::On) {
            e.status = Status::Applied;
            e.detail = "router/DRC keep-out (tracks) on " + layers + "; sidecar .kicad_dru rule";
          } else {
            e.detail = std::string(mode == Mode::Report ? "report mode" : "soft mode adds no hard constraints") + "; keep-out on " + layers +
                       " is used with --component-rules on";
          }
        } else {
          e.detail = not_built_reason(r);
          for (const auto& s : ko_missing)
            if (s.starts_with(r.id + " " + b.footprints[static_cast<std::size_t>(in.anchor)].reference + ":")) e.detail = s.substr(s.find(':') + 2);
        }
      } else if (r.kind == "impedance") {
        // P4 (report only): widths and gaps per routing layer from the stackup; never guessed without one (§3.6).
        e.impedance = plan_impedance(b, rules, rp);
        const std::string computed = impedance_text(*e.impedance);
        if (const std::string cls = board_pair_class(b, rules, in, r); !cls.empty()) {
          e.status = Status::SatisfiedByBoard;
          const model::NetClass* nc = rules->find_class(cls);
          std::string zc;
          if (nc && e.impedance->zdiff > 0)
            if (const double z = pair_impedance(b, 0, nc->diff_pair_width, nc->diff_pair_gap); z > 0) {
              char buf[64];
              std::snprintf(buf, sizeof buf, ", %.0f Ω diff on %s by this model", z, b.copper_name(0).c_str());
              zc = buf;
            }
          e.detail = "net class " + cls + (nc ? " (diff pair " + fmt_mm3(nc->diff_pair_width) + " mm / gap " + fmt_mm3(nc->diff_pair_gap) + " mm" + zc + ")" : "") +
                     (e.impedance->computed ? "; for comparison, " + computed : "");
        } else {
          e.detail = computed;
        }
      } else if (r.kind == "width_for_current" && rp.params.contains("current_a")) {
        e.current = plan_current(b, rp);
        e.detail = current_text(*e.current);
      } else if (r.kind == "diff_pair") {
        const Role* dp = in.role("dp");
        const Role* dm = in.role("dm");
        const bool usb_pair = r.id == "USB2-02" && dp && dm && dp->nets.size() == 1 && dm->nets.size() == 1;
        if (const std::string cls = board_pair_class(b, rules, in, r); !cls.empty()) {
          e.status = Status::SatisfiedByBoard;
          e.detail = "net class " + cls;
        } else if (usb_pair && mode != Mode::Report) {
          e.status = Status::Applied;
          e.detail = "router: D+/D- routed coupled first (falls back to single tracks); intra-pair skew not checked yet";
        } else if (usb_pair) {
          e.detail = "report mode (tracemaker route --component-rules soft routes the pair coupled first)";
        } else {
          e.detail = not_built_reason(r);
        }
      } else if (r.kind == "order" && (r.id == "USB2-08")) {
        e.detail = "connector-ESD leg placed by USB2-07; the routing order itself is not built yet (doc 15 P3)";
      } else if (r.kind == "edge") {
        e.detail = "measured only (edge connectors stay fixed at the edge; no edge attraction for other parts yet)";
      } else {
        e.detail = measured_rule ? "measured only" : not_built_reason(r);
      }
      if (user)
        for (const auto& n : user->notes) e.detail += "; overridden by user: " + n;
      ev.rules.push_back(std::move(e));
    }
  }
  return ev;
}

}  // namespace tmk::crules
