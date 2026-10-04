// The component-rule report (doc 15 §7): what was detected, with which confidence and roles, and each rule's
// status. Text for the terminal, JSON for tools (bench/crules_detect.py, bench/place_intent.py).
#include <array>
#include <cstdio>
#include <map>
#include <sstream>

#include "crules/engine.hpp"

namespace tmk::crules {

namespace {

constexpr int kReportDetailMax = 6;  // more instances of one category are summarised in the text report

std::string fmt_mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

std::string role_text(const model::Board& b, const Role& r) {
  std::string s;
  auto add = [&](const std::string& x) { s += (s.empty() ? "" : ",") + x; };
  if (r.name == "antenna_region") return "footprint keep-out";
  if (!r.pads.empty() && (r.parts.empty() || r.name == "pin" || r.name == "xin" || r.name == "xout" || r.name == "vin" || r.name == "vout" || r.name == "fb")) {
    for (int p : r.pads) {
      const auto& pd = b.pads[static_cast<std::size_t>(p)];
      add(b.footprints[static_cast<std::size_t>(pd.footprint)].reference + "." + pd.number);
    }
    return s;
  }
  if (!r.parts.empty()) {
    for (int f : r.parts) add(b.footprints[static_cast<std::size_t>(f)].reference);
    return s;
  }
  for (auto n : r.nets) add(b.nets[static_cast<std::size_t>(n)].name);
  return s;
}

nlohmann::json role_json(const model::Board& b, const Role& r) {
  nlohmann::json j = nlohmann::json::object();
  nlohmann::json parts = nlohmann::json::array(), pads = nlohmann::json::array(), nets = nlohmann::json::array();
  for (int f : r.parts) parts.push_back(b.footprints[static_cast<std::size_t>(f)].reference);
  if (r.name != "antenna_region")
    for (int p : r.pads) {
      const auto& pd = b.pads[static_cast<std::size_t>(p)];
      pads.push_back(b.footprints[static_cast<std::size_t>(pd.footprint)].reference + "." + pd.number);
    }
  for (auto n : r.nets) nets.push_back(b.nets[static_cast<std::size_t>(n)].name);
  j["parts"] = parts;
  j["pads"] = pads;
  j["nets"] = nets;
  return j;
}

std::string measure_text(const Measure& m) {
  if (m.limit_mm > 0) return (m.met ? "met (" : "VIOLATED (") + fmt_mm(m.value_mm) + " mm, limit " + fmt_mm(m.limit_mm) + " mm)";
  return "measured " + fmt_mm(m.value_mm) + " mm";
}

// Rules not built yet are listed once per category, not once per instance.
bool generic_not_built(const EffectiveRule& e) {
  return e.status == Status::NotApplied && !e.measure &&
         (e.detail.find("(doc 15 P") != std::string::npos || e.detail.find("not built") != std::string::npos ||
          e.detail.find("not generated for this category") != std::string::npos || e.detail.find("not bound to pins for this category") != std::string::npos);
}

}  // namespace

std::string report_text(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev) {
  std::ostringstream s;
  s << "Component rules (catalogue v" << cat.version << ", mode " << mode_name(ev.mode) << ", thresholds apply " << cat.apply << " / suggest "
    << cat.suggest << ")\n";
  if (!det.board_has_pin_names)
    s << "  board has no pad pin names (KiCad 5 file): binding uses net names, pad numbers and topology; categories that need pin names are capped at "
      << kNoPinNameCap << "\n";
  if (ev.mode == Mode::Off) return s.str();
  s << "  " << stackup_summary(b, cat) << "\n";
  std::map<int, std::vector<const EffectiveRule*>> by_inst;
  for (const auto& e : ev.rules) by_inst[e.instance].push_back(&e);
  std::map<int, std::map<std::string, std::string>> not_built;  // category -> rule id -> reason
  std::map<int, int> per_cat;
  for (const auto& in : det.instances) ++per_cat[in.category];
  int summarised = -1;
  for (std::size_t ii = 0; ii < det.instances.size(); ++ii) {
    const Instance& in = det.instances[ii];
    const Category& C = cat.categories[static_cast<std::size_t>(in.category)];
    // Many instances of one category (decoupling caps, pin headers): one summary per category and rule.
    if (per_cat[in.category] > kReportDetailMax) {
      if (summarised == in.category) continue;
      summarised = in.category;
      std::string refs;
      int lo = 100, hi = 0;
      std::map<std::string, std::array<int, 4>> agg;  // rule -> applied, met, violated, other
      std::map<std::string, std::pair<double, std::string>> worst;
      for (std::size_t jj = ii; jj < det.instances.size() && det.instances[jj].category == in.category; ++jj) {
        const Instance& x = det.instances[jj];
        refs += " " + b.footprints[static_cast<std::size_t>(x.anchor)].reference;
        lo = std::min(lo, x.confidence);
        hi = std::max(hi, x.confidence);
        for (const EffectiveRule* e : by_inst[static_cast<int>(jj)]) {
          if (e->status == Status::Advisory && !e->measure) continue;
          if (generic_not_built(*e)) {
            not_built[in.category][e->spec->id] = e->spec->kind + ": " + e->detail;
            continue;
          }
          auto& a = agg[e->spec->id];
          a[0] += e->status == Status::Applied ? 1 : 0;
          if (e->measure) {
            a[e->measure->met ? 1 : 2] += 1;
            auto& w = worst[e->spec->id];
            if (!e->measure->met && e->measure->value_mm > w.first)
              w = {e->measure->value_mm, b.footprints[static_cast<std::size_t>(x.anchor)].reference};
          } else {
            a[3] += 1;
          }
        }
      }
      s << "  " << C.id << ": " << per_cat[in.category] << " instances, confidence " << lo << (lo != hi ? "-" + std::to_string(hi) : "") << ":" << refs
        << "\n";
      for (const auto& [id, a] : agg) {
        if (a[0] + a[1] + a[2] == 0) continue;  // nothing applied or measured (e.g. DEC-07 without a VREF pin)
        s << "     " << id << "  applied " << a[0] << ", met " << a[1] << ", violated " << a[2];
        if (a[3]) s << ", not measured " << a[3];
        if (const auto w = worst.find(id); w != worst.end() && w->second.first > 0) s << " (worst " << w->second.second << " " << fmt_mm(w->second.first) << " mm)";
        s << "\n";
      }
      continue;
    }
    const auto& fp = b.footprints[static_cast<std::size_t>(in.anchor)];
    char head[256];
    std::snprintf(head, sizeof head, "  %-17s %-6s %-28.28s confidence %3d%s", C.id.c_str(), fp.reference.c_str(), fp.value.c_str(), in.confidence,
                  in.capped ? " (capped)" : in.confidence < cat.apply ? " (suggest)" : "");
    s << head;
    std::string roles;
    for (const auto& r : in.roles) roles += " " + r.name + "=" + role_text(b, r);
    if (!roles.empty()) s << "  roles:" << roles;
    s << "\n";
    int advisory = 0;
    for (const EffectiveRule* e : by_inst[static_cast<int>(ii)]) {
      if (e->status == Status::Advisory && !e->measure) {
        ++advisory;
        continue;
      }
      if (generic_not_built(*e)) {
        not_built[in.category][e->spec->id] = e->spec->kind + ": " + e->detail;
        continue;
      }
      char line[128];
      std::snprintf(line, sizeof line, "     %-9s %-15s %-9s ", e->spec->id.c_str(), e->spec->kind.c_str(), severity_name(e->severity));
      // A detail that already says "not applied" (kept verbatim for doc 15 §3.6) is not prefixed again.
      if (e->status == Status::NotApplied && e->detail.starts_with("not applied")) s << line << e->detail;
      else s << line << status_name(e->status) << ": " << e->detail;
      if (e->measure) s << "  " << measure_text(*e->measure);
      s << "\n";
    }
    if (advisory) s << "     (+" << advisory << " advisory rule(s), see --json)\n";
  }
  if (!not_built.empty()) {
    s << "  not applied (not built yet), per category:\n";
    for (const auto& [ci, rules] : not_built) {
      s << "    " << cat.categories[static_cast<std::size_t>(ci)].id << ":";
      for (const auto& [id, why] : rules) s << " " << id;
      s << "\n";
    }
  }
  if (!det.possible.empty()) {
    s << "  possible (below " << cat.suggest << ", superseded or not bound; reported only):\n";
    for (const auto& in : det.possible) {
      s << "    " << b.footprints[static_cast<std::size_t>(in.anchor)].reference << " " << cat.categories[static_cast<std::size_t>(in.category)].id << " ("
        << in.confidence;
      for (const auto& e : in.evidence) s << ", " << e;
      s << ")";
      if (!in.superseded_by.empty()) s << " - " << in.superseded_by;
      s << "\n";
    }
  }
  return s.str();
}

nlohmann::json report_json(const model::Board& b, const Catalogue& cat, const Detection& det, const Evaluation& ev) {
  using nlohmann::json;
  json j;
  j["catalogue_version"] = cat.version;
  j["mode"] = mode_name(ev.mode);
  j["thresholds"] = {{"apply", cat.apply}, {"suggest", cat.suggest}};
  j["board_has_pin_names"] = det.board_has_pin_names;
  j["stackup"] = stackup_json(b, cat);
  std::map<int, std::vector<const EffectiveRule*>> by_inst;
  for (const auto& e : ev.rules) by_inst[e.instance].push_back(&e);
  auto inst_json = [&](const Instance& in) {
    const auto& fp = b.footprints[static_cast<std::size_t>(in.anchor)];
    json x = {{"category", cat.categories[static_cast<std::size_t>(in.category)].id},
              {"anchor", fp.reference},
              {"value", fp.value},
              {"lib_id", fp.lib_id},
              {"confidence", in.confidence},
              {"capped", in.capped},
              {"evidence", in.evidence}};
    json roles = json::object();
    for (const auto& r : in.roles) roles[r.name] = role_json(b, r);
    x["roles"] = roles;
    if (!in.superseded_by.empty()) x["note"] = in.superseded_by;
    return x;
  };
  json insts = json::array();
  std::map<std::string, int> counts;
  for (std::size_t ii = 0; ii < det.instances.size(); ++ii) {
    json x = inst_json(det.instances[ii]);
    json rules = json::array();
    for (const EffectiveRule* e : by_inst[static_cast<int>(ii)]) {
      json r = {{"id", e->spec->id}, {"kind", e->spec->kind}, {"severity", severity_name(e->severity)}, {"status", status_name(e->status)}, {"detail", e->detail}};
      if (e->impedance) r["impedance"] = impedance_json(*e->impedance);
      if (e->current) r["current"] = current_json(*e->current);
      if (e->measure) {
        r["measured_mm"] = e->measure->value_mm;
        r["limit_mm"] = e->measure->limit_mm;
        r["met"] = e->measure->met;
        r["measure"] = e->measure->what;
      }
      ++counts[status_name(e->status)];
      rules.push_back(std::move(r));
    }
    x["rules"] = rules;
    insts.push_back(std::move(x));
  }
  j["instances"] = insts;
  json poss = json::array();
  for (const auto& in : det.possible) poss.push_back(inst_json(in));
  j["possible"] = poss;
  j["rule_status_counts"] = counts;
  // Generated keep-outs (used by the router with --component-rules on), polygon in mm, for the viewer and tests.
  json kos = json::array();
  for (const auto& k : generate_keepouts(b, cat, det)) {
    json poly = json::array();
    for (const auto& q : k.zone.outline.front()) poly.push_back({nm_to_mm(q.x), nm_to_mm(q.y)});
    kos.push_back({{"name", k.zone.name}, {"rule", k.rule}, {"ref", k.ref}, {"layers", k.zone.layers}, {"polygon_mm", poly}});
  }
  j["keepouts"] = kos;
  return j;
}

}  // namespace tmk::crules
