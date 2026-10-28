// SPDX-License-Identifier: GPL-3.0-or-later
// The run report renderer (doc 18 D3). The report is built as a small document (sections of paragraphs and tables whose
// values carry their JSON pointer) and written as HTML or Markdown from the same document, so the two never disagree.
#include "report/render.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace tmk::report {
namespace {

using nlohmann::json;

// A piece of text: the renderer's own words (src empty) or a value of the summary (src = its JSON pointer).
struct Run {
  std::string text;
  std::string src;
};
using Para = std::vector<Run>;

struct Table {
  std::vector<std::string> head;
  std::vector<std::vector<Para>> rows;
  std::string fields;  // where the rows come from, e.g. "/stuck/unrouted/<row>" (shown as the table's caption)
};

struct Block {
  enum Kind { Text, Warn, Tab } kind = Text;
  Para para;
  Table table;
};

struct Section {
  std::string id, title;
  std::vector<Block> blocks;
};

// Read access to the summary in pointer form.
struct Src {
  const json& s;
  bool has(const std::string& p) const {
    const json::json_pointer jp(p);
    return s.contains(jp) && !s.at(jp).is_null();
  }
  const json& at(const std::string& p) const { return s.at(json::json_pointer(p)); }
  Run v(const std::string& p) const { return has(p) ? Run{value_text(at(p)), p} : Run{"n/a", ""}; }
  std::size_t size(const std::string& p) const { return has(p) && at(p).is_array() ? at(p).size() : 0; }
  bool truthy(const std::string& p) const { return has(p) && ((at(p).is_boolean() && at(p).get<bool>()) || (at(p).is_number() && at(p).get<double>() != 0)); }
  double num(const std::string& p) const { return has(p) && at(p).is_number() ? at(p).get<double>() : 0.0; }
};

Run t(std::string s) { return {std::move(s), ""}; }
std::string idx(const std::string& base, std::size_t i) { return base + "/" + std::to_string(i); }

// What each failure reason code (route::failure_reason_code) means, in a designer's words.
std::string reason_text(const std::string& code) {
  static const std::map<std::string, std::string> k = {
      {"boxed_in", "Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use."},
      {"layer_limit", "The only way out needs a track on a layer that takes no tracks (--no-tracks-on or the rules)."},
      {"rip_limit", "Routing it would have meant ripping up protected or too many routed connections, and the router declined."},
      {"exact_check", "Paths were found on the routing lattice but each failed the exact geometric check: a clearance or width the lattice does not see."},
      {"ripped_not_rerouted", "It was routed, then ripped up to make room for another net, and could not be routed again."},
      {"out_of_budget", "The work budget ran out while it was being searched; a larger --work may route it."},
      {"rolled_back", "A route was found but its transaction was rolled back because the board scored worse with it."},
      {"not_attempted", "The run ended (work or time) before the router tried it again."},
      {"no_legal_path", "The search found no path that keeps the board's rules around the copper already there. This is not a proof that none exists."},
  };
  const auto it = k.find(code);
  return it == k.end() ? std::string("No description for this cause.") : it->second;
}

// ---- building the document from the summary ----

class Builder {
 public:
  explicit Builder(const json& s) : src_{s} {}
  std::vector<Section> build();

 private:
  void text(Section& s, Para p) { s.blocks.push_back({Block::Text, std::move(p), {}}); }
  void warn(Section& s, Para p) { s.blocks.push_back({Block::Warn, std::move(p), {}}); }
  void table(Section& s, Table t) {
    if (!t.rows.empty()) s.blocks.push_back({Block::Tab, {}, std::move(t)});
  }
  // A two-column table of named values that exist in the summary.
  Table kv(const std::vector<std::pair<std::string, std::string>>& rows) {
    Table tb{{"", "Value"}, {}, ""};
    for (const auto& [label, p] : rows)
      if (src_.has(p)) tb.rows.push_back({{t(label)}, {src_.v(p)}});
    return tb;
  }
  // A table with one row per element of the array at `base`; columns are (heading, field of the element).
  Table rows(const std::string& base, const std::vector<std::pair<std::string, std::string>>& cols, std::size_t limit = 1000) {
    Table tb{{}, {}, base + "/<row>"};
    for (const auto& c : cols) tb.head.push_back(c.first);
    for (std::size_t i = 0; i < src_.size(base) && i < limit; ++i) {
      std::vector<Para> r;
      for (const auto& c : cols) r.push_back(cell(idx(base, i) + "/" + c.second));
      tb.rows.push_back(std::move(r));
    }
    return tb;
  }
  // A value, or each element of an array of values joined by commas.
  Para cell(const std::string& p) {
    if (!src_.has(p)) return {t("")};
    if (!src_.at(p).is_array()) return {src_.v(p)};
    Para out;
    for (std::size_t i = 0; i < src_.at(p).size(); ++i) {
      if (i) out.push_back(t(", "));
      out.push_back(src_.v(idx(p, i)));
    }
    return out;
  }
  Section header();
  Section result();
  Section open();
  // What the board as written leaves unconnected (the DRC after the route). With copper zones the router counts a pad joined
  // once its track reaches the old fill; KiCad refills the zones for the new copper and judges that.
  void unconnected_after(Section& s);
  Section rules();
  Section si();
  Section drc();
  Section options();

  Src src_;
};

Section Builder::header() {
  Section s{"summary", "", {}};
  if (src_.has("/run/version")) {
    Para p{t("TraceMaker "), src_.v("/run/version")};
    if (src_.has("/run/date")) p.insert(p.end(), {t(", "), src_.v("/run/date")});
    if (src_.has("/run/input")) p.insert(p.end(), {t(". Input "), src_.v("/run/input")});
    if (src_.has("/run/output")) p.insert(p.end(), {t(", output "), src_.v("/run/output")});
    p.push_back(t("."));
    text(s, p);
  }
  // Two verdicts: the router's (connections it planned and joined) and, with zones, the refilled board's (what KiCad will judge).
  const bool soft = src_.has("/unconnected_after_refill");
  const bool routed_all = src_.has("/open") ? src_.num("/open") == 0 : src_.num("/routed") == src_.num("/connections");
  const bool joined_all = !soft || src_.num("/unconnected_after_refill") == 0;
  if (routed_all && joined_all) {
    text(s, {t("Every connection was routed: "), src_.v("/routed"), t(" of "), src_.v("/connections"), t(".")});
  } else if (routed_all) {
    text(s, {t("The router joined every connection it planned: "), src_.v("/routed"), t(" of "), src_.v("/connections"), t(".")});
  } else if (src_.has("/open")) {
    warn(s, {src_.v("/open"), t(" of "), src_.v("/connections"), t(" connections are open; "), src_.v("/completion_pct"), t(" % are routed.")});
  } else {
    warn(s, {src_.v("/routed"), t(" of "), src_.v("/connections"), t(" connections are routed.")});
  }
  if (soft && !joined_all)
    warn(s, {t("After the zones are refilled for the new copper, "), src_.v("/unconnected_after_refill"),
             t(src_.num("/unconnected_after_refill") == 1 ? " item is" : " items are"),
             t(" unconnected: the board is not finished. See What is open and why.")});
  else if (soft)
    text(s, {t("After the zones are refilled for the new copper, "), src_.v("/unconnected_after_refill"),
              t(src_.num("/unconnected_after_refill") == 1 ? " item is unconnected." : " items are unconnected.")});
  if (src_.has("/drc/added_routing_errors")) {
    if (src_.num("/drc/added_routing_errors") > 0)
      warn(s, {t("The route added "), src_.v("/drc/added_routing_errors"),
               t(src_.num("/drc/added_routing_errors") == 1 ? " DRC error" : " DRC errors"), t(" of types routing can cause; see the DRC section.")});
    else
      text(s, {t("The route added "), src_.v("/drc/added_routing_errors"), t(" DRC errors of types routing can cause.")});
  }
  if (src_.truthy("/time_limited"))
    warn(s, {t("The time limit ("), src_.v("/run/time_limit_s"), t(" s), not the work budget, ended the search: this result depends on how fast the machine "
             "was, and another run may differ. Give a larger --time for a repeatable run.")});
  if (src_.num("/rules/not_read_count") > 0)
    warn(s, {src_.v("/rules/not_read_count"),
             t(src_.num("/rules/not_read_count") == 1 ? " rule or setting could not be read; the route may break it. See Rules."
                                                       : " rules or settings could not be read; the route may break them. See Rules.")});
  return s;
}

Section Builder::result() {
  Section s{"result", "Result", {}};
  table(s, kv({{"Connections", "/connections"},
               {"Routed", "/routed"},
               {"Open", "/open"},
               {"Routed, %", "/completion_pct"},
               {"Unconnected after zone refill", "/unconnected_after_refill"},
               {"Tracks added", "/tracks"},
               {"Vias added", "/vias"},
               {"Winning variant", "/variant_name"},
               {"Lattice pitch, mm", "/pitch_mm"},
               {"Search expansions, all variants", "/work_all_variants/total/expansions"},
               {"Wall time, s", "/seconds"}}));
  return s;
}

Section Builder::open() {
  Section s{"open", "What is open and why", {}};
  const bool none_open = src_.has("/open") ? src_.num("/open") == 0 : src_.num("/routed") == src_.num("/connections");
  const bool gaps = src_.num("/drc/after/unconnected") > 0 && src_.size("/drc/after/unconnected_items") > 0;
  if (!src_.has("/stuck")) {
    if (none_open && !gaps) {
      text(s, {t("Nothing is open.")});
    } else if (none_open) {
      text(s, {t("The router joined every connection it planned.")});
    } else {
      text(s, {t("The router's own explanation of each open connection:")});
      Table tb{{"Connection and why"}, {}, "/failures/<row>"};
      for (std::size_t i = 0; i < src_.size("/failures"); ++i) tb.rows.push_back({{src_.v(idx("/failures", i))}});
      table(s, tb);
      text(s, {t("This summary has no stuck report; route with --report for the causes, the crowding and the escape analysis.")});
    }
    if (gaps) unconnected_after(s);
    return s;
  }
  if (src_.size("/stuck/unrouted_by_reason") > 0) {
    text(s, {t("Open connections by cause:")});
    Table tb{{"Cause", "Connections", "What it means"}, {}, "/stuck/unrouted_by_reason/<row>"};
    for (std::size_t i = 0; i < src_.size("/stuck/unrouted_by_reason"); ++i) {
      const auto b = idx("/stuck/unrouted_by_reason", i);
      tb.rows.push_back({{src_.v(b + "/name")}, {src_.v(b + "/count")}, {t(reason_text(src_.at(b + "/name").get<std::string>()))}});
    }
    table(s, tb);
  }
  if (src_.size("/stuck/result/cuts/unroutable") > 0) {
    warn(s, {t("Cut proof: more nets must cross these lines than tracks fit through them, so no routing completes the board without moving parts "
               "or adding layers.")});
    table(s, rows("/stuck/result/cuts/unroutable", {{"Axis", "axis"}, {"At, mm", "at_mm"}, {"Nets that must cross", "demand"}, {"Tracks that fit", "capacity"}}));
  }
  bool dead = false;
  for (std::size_t i = 0; i < src_.size("/stuck/escape_analysis"); ++i) dead = dead || src_.num(idx("/stuck/escape_analysis", i) + "/cannot_escape") > 0;
  if (dead) {
    warn(s, {t("Some pins cannot leave their package at all under the board's rules; no amount of routing effort connects them.")});
    // One row per package and cause, with the pins it traps; packages whose pins all escape are left out.
    Table tb{{"Part", "Pitch, mm", "Pins that cannot escape", "Pads", "Why", "What would help"}, {}, "/stuck/escape_analysis/<row>/dead/<row>"};
    for (std::size_t i = 0; i < src_.size("/stuck/escape_analysis"); ++i) {
      const auto p = idx("/stuck/escape_analysis", i);
      for (std::size_t k = 0; k < src_.size(p + "/dead"); ++k) {
        const auto d = idx(p + "/dead", k);
        tb.rows.push_back({{src_.v(p + "/part")}, {src_.v(p + "/pitch_mm")}, {src_.v(d + "/count")}, cell(d + "/pads"), cell(d + "/cause"), cell(p + "/hint")});
      }
    }
    table(s, tb);
  }
  if (src_.size("/stuck/unrouted_by_part") > 0) {
    text(s, {t("Parts with the most open connection ends:")});
    table(s, rows("/stuck/unrouted_by_part", {{"Part", "name"}, {"Open ends", "count"}}));
  }
  if (src_.size("/stuck/unrouted") > 0) {
    text(s, {t("Each open connection, with the router's last explanation and the nets whose copper was in the way:")});
    Table tb{{"Net", "From", "To", "Length, mm", "Cause", "Why", "In the way"}, {}, "/stuck/unrouted/<row>"};
    for (std::size_t i = 0; i < src_.size("/stuck/unrouted"); ++i) {
      const auto b = idx("/stuck/unrouted", i);
      Para to = src_.truthy(b + "/to/zone") ? Para{t("zone fill")} : Para{src_.v(b + "/to/part"), t(" pad "), src_.v(b + "/to/pad")};
      tb.rows.push_back({cell(b + "/net"), {src_.v(b + "/from/part"), t(" pad "), src_.v(b + "/from/pad")}, to, cell(b + "/length_mm"),
                         cell(b + "/reason"), cell(b + "/why"), cell(b + "/blockers")});
    }
    table(s, tb);
  }
  if (src_.has("/stuck/unrouted_not_shown"))
    text(s, {src_.v("/stuck/unrouted_not_shown"), t(" more open connections are not listed here; the summary's failures list has them all.")});
  if (gaps) unconnected_after(s);
  return s;
}

void Builder::unconnected_after(Section& s) {
  text(s, {t("Unconnected on the board as written, by the DRC after the route ("), src_.v("/drc/after/unconnected"),
           t(" items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new "
             "tracks cut off no longer joins the pads the router counted as joined through it.")});
  Table tb{{"Missing connection", "From", "To", "X, mm", "Y, mm"}, {}, "/drc/after/unconnected_items/<row>"};
  for (std::size_t i = 0; i < src_.size("/drc/after/unconnected_items"); ++i) {
    const auto b = idx("/drc/after/unconnected_items", i);
    tb.rows.push_back({cell(b + "/what"), cell(b + "/from"), cell(b + "/to"), cell(b + "/x_mm"), cell(b + "/y_mm")});
  }
  table(s, tb);
  if (src_.has("/drc/after/unconnected_items_not_shown"))
    text(s, {src_.v("/drc/after/unconnected_items_not_shown"), t(" more are not listed.")});
}

Section Builder::rules() {
  Section s{"rules", "Rules", {}};
  if (!src_.has("/rules")) {
    text(s, {t("This summary has no rules section; route with --report.")});
    return s;
  }
  Table files{{"File", "Name", "Found"}, {}, "/rules/files"};
  files.rows.push_back({{t("Project")}, {src_.v("/rules/files/project")}, {src_.v("/rules/files/project_found")}});
  files.rows.push_back({{t("Custom rules")}, {src_.v("/rules/files/custom_rules")}, {src_.v("/rules/files/custom_rules_found")}});
  table(s, files);
  if (src_.num("/rules/not_read_count") > 0) {
    warn(s, {t("Not read: the router could not use these, so the routed board may break them (it treats what it cannot read conservatively where it can).")});
    table(s, rows("/rules/not_read", {{"From", "source"}, {"What", "text"}}));
  } else {
    text(s, {t("The readers reported nothing they could not read.")});
  }
  text(s, {t("Board minimums:")});
  table(s, kv({{"Clearance, mm", "/rules/minimums/clearance_mm"},
               {"Track width, mm", "/rules/minimums/track_width_mm"},
               {"Via diameter, mm", "/rules/minimums/via_diameter_mm"},
               {"Via annular width, mm", "/rules/minimums/via_annular_width_mm"},
               {"Drill, mm", "/rules/minimums/drill_mm"},
               {"Hole clearance, mm", "/rules/minimums/hole_clearance_mm"},
               {"Hole to hole, mm", "/rules/minimums/hole_to_hole_mm"},
               {"Copper to edge, mm", "/rules/minimums/copper_edge_clearance_mm"},
               {"Blind and buried vias", "/rules/minimums/blind_buried_vias"},
               {"Microvias", "/rules/minimums/microvias"}}));
  text(s, {t("Net classes:")});
  table(s, rows("/rules/net_classes", {{"Class", "name"},
                                       {"Nets", "nets"},
                                       {"Clearance, mm", "clearance_mm"},
                                       {"Track, mm", "track_width_mm"},
                                       {"Via, mm", "via_diameter_mm"},
                                       {"Drill, mm", "via_drill_mm"},
                                       {"Pair width, mm", "diff_pair_width_mm"},
                                       {"Pair gap, mm", "diff_pair_gap_mm"}}));
  if (src_.size("/rules/custom_rules") > 0) {
    text(s, {t("Custom rules ("), src_.v("/rules/custom_rule_count"), t("):")});
    table(s, rows("/rules/custom_rules", {{"Rule", "name"}, {"Layer", "layer"}, {"Constraints", "constraints"}, {"Condition", "condition"}}));
  } else {
    text(s, {t("No custom rules.")});
  }
  if (src_.size("/component_rule_net_classes") > 0) {
    text(s, {t("Net classes from the component rules:")});
    table(s, rows("/component_rule_net_classes",
                  {{"Class", "name"}, {"Rule", "rule"}, {"Track, mm", "track_width_mm"}, {"Pair gap, mm", "diff_pair_gap_mm"}, {"Applied", "applied"}, {"Nets", "nets"}, {"Detail", "detail"}}));
  }
  return s;
}

Section Builder::si() {
  Section s{"si", "Signal-integrity audit", {}};
  const bool any = src_.has("/critical_nets") || src_.has("/pairs") || src_.has("/buses") || src_.has("/return_paths");
  if (!any) {
    // The options as given say whether an audit was asked for and found nothing to audit, or was not asked for.
    bool asked = false;
    for (std::size_t i = 0; i < src_.size("/run/options"); ++i) {
      const auto& n = src_.at(idx("/run/options", i) + "/name");
      for (const char* o : {"--si-rules", "--critical-nets", "--critical-auto", "--diff-pairs", "--bus", "--buses", "--return-paths", "--return-path-classes",
                            "--return-path-pairs"})
        asked = asked || (n.is_string() && n.get<std::string>() == o);
    }
    if (asked)
      text(s, {t("The options that turn the audit on were given, but the board has nothing for them: no critical nets, differential pairs, buses or "
                 "return-path nets were found.")});
    else
      text(s, {t("No audit was asked for. --si-rules, --critical-nets, --diff-pairs, --bus or --buses, and --return-paths turn its parts on.")});
    return s;
  }
  if (src_.has("/critical_nets")) {
    text(s, {t("Critical nets: "), src_.v("/critical_nets/totals/nets"), t(" nets, "), src_.v("/critical_nets/totals/vias"), t(" vias, "),
             src_.v("/critical_nets/totals/void_crossings"), t(" crossings of a reference-plane void.")});
    if (src_.has("/critical_nets/totals/missing_stitches")) {
      Para p{t("Layer changes without a return via within reach: "), src_.v("/critical_nets/totals/missing_stitches")};
      if (src_.has("/critical_nets/totals/missing_stitches_before_stitching"))
        p.insert(p.end(), {t(" (before the stitching pass of --si-rules: "), src_.v("/critical_nets/totals/missing_stitches_before_stitching"), t(")")});
      p.push_back(t("."));
      text(s, p);
    }
    table(s, rows("/critical_nets/nets", {{"Net", "name"},
                                          {"Tier", "tier"},
                                          {"Why critical", "why"},
                                          {"Vias", "vias"},
                                          {"Length, mm", "length_mm"},
                                          {"Unrouted", "unrouted"},
                                          {"Void crossings", "void_crossings"},
                                          {"Unstitched layer changes", "missing_stitches"},
                                          {"Warnings", "warnings"}}));
  }
  if (src_.has("/pairs")) {
    text(s, {t("Differential pairs:")});
    table(s, rows("/pairs", {{"Net", "net_a"},
                             {"Partner", "net_b"},
                             {"Length, mm", "length_mm"},
                             {"Coupled share", "coupled_share"},
                             {"Gap, mm", "gap_mm"},
                             {"Target gap, mm", "target_gap_mm"},
                             {"Skew, mm", "skew_mm"}}));
  }
  if (src_.has("/buses")) {
    text(s, {t("Buses:")});
    table(s, rows("/buses", {{"Bus", "name"},
                             {"Nets", "nets"},
                             {"Routed", "routed"},
                             {"On the bus's layers", "followed"},
                             {"Left the bus", "left"},
                             {"Skew, mm", "skew_mm"},
                             {"Skew limit, mm", "skew_limit_mm"},
                             {"Within limit", "within_skew"}}));
  }
  if (src_.has("/return_paths")) {
    text(s, {t("Return paths:")});
    table(s, rows("/return_paths/nets", {{"Net", "net"},
                                         {"Signal, mm", "signal_mm"},
                                         {"Detour, mm", "detour_mm"},
                                         {"Largest separation, mm", "max_separation_mm"},
                                         {"Void crossings", "void_crossings"},
                                         {"Splits", "splits"},
                                         {"Missing stitches", "missing_stitches"},
                                         {"Warnings", "warnings"}}));
    for (std::size_t i = 0; i < src_.size("/return_paths/notes"); ++i) text(s, {src_.v(idx("/return_paths/notes", i))});
  }
  return s;
}

Section Builder::drc() {
  Section s{"drc", "DRC before and after", {}};
  if (!src_.has("/drc")) {
    text(s, {t("This summary has no DRC comparison; route with --report.")});
    return s;
  }
  text(s, {t("Checked with "), src_.v("/drc/checker"), t(" (KiCad's violation names); zones refilled first: "), src_.v("/drc/zones_refilled"),
           t(". The comparison counts violations by type; sign off with kicad-cli pcb drc.")});
  Table tb{{"", "Before", "After"}, {}, "/drc/before, /drc/after"};
  tb.rows.push_back({{t("Errors")}, {src_.v("/drc/before/errors")}, {src_.v("/drc/after/errors")}});
  tb.rows.push_back({{t("Warnings")}, {src_.v("/drc/before/warnings")}, {src_.v("/drc/after/warnings")}});
  tb.rows.push_back({{t("Unconnected items")}, {src_.v("/drc/before/unconnected")}, {src_.v("/drc/after/unconnected")}});
  table(s, tb);
  Para added{t("Errors added of types routing can cause: "), src_.v("/drc/added_routing_errors"), t("; violations gone: "), src_.v("/drc/removed"), t(".")};
  if (src_.num("/drc/added_routing_errors") > 0) warn(s, std::move(added));
  else text(s, std::move(added));
  table(s, rows("/drc/types", {{"Type", "type"}, {"Severity", "severity"}, {"Routing", "routing"}, {"Before", "before"}, {"After", "after"}, {"Added", "added"}}));
  // A note the check of both boards gave (no project file, say) is printed once, from the before side.
  for (std::size_t i = 0; i < src_.size("/drc/before/notes"); ++i) text(s, {t("Note: "), src_.v(idx("/drc/before/notes", i))});
  for (std::size_t i = 0; i < src_.size("/drc/after/notes"); ++i) {
    const auto& note = src_.at(idx("/drc/after/notes", i));
    bool seen = false;
    for (std::size_t j = 0; j < src_.size("/drc/before/notes"); ++j) seen = seen || src_.at(idx("/drc/before/notes", j)) == note;
    if (!seen) text(s, {t("Note: "), src_.v(idx("/drc/after/notes", i))});
  }
  return s;
}

Section Builder::options() {
  Section s{"options", "Options and seed", {}};
  table(s, kv({{"Seed", "/run/seed"},
               {"Work budget per variant, expansions", "/run/work_budget"},
               {"Time limit, s", "/run/time_limit_s"},
               {"Stopped by the time limit", "/time_limited"},
               {"Variants", "/run/variants"},
               {"Threads", "/run/threads"}}));
  if (src_.size("/run/options") > 0) {
    text(s, {t("Options given (command line and --config):")});
    table(s, rows("/run/options", {{"Option", "name"}, {"Value", "value"}}));
  }
  if (src_.size("/variants") > 0) {
    text(s, {t("Portfolio variants:")});
    table(s, rows("/variants", {{"Variant", "variant"}, {"Name", "name"}, {"Routed", "routed"}, {"Seconds", "seconds"}}));
  }
  if (src_.size("/advice/advisor/rounds") > 0) {
    text(s, {t("Advisor rounds:")});
    table(s, rows("/advice/advisor/rounds", {{"Round", "round"}, {"Routed before", "routed_before"}, {"Routed after", "routed_after"}, {"Kept", "adopted"}}));
  }
  return s;
}

std::vector<Section> Builder::build() { return {header(), result(), open(), rules(), si(), drc(), options()}; }

// ---- writers ----

std::string html_escape(const std::string& in) {
  std::string o;
  for (const char c : in) switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  return o;
}

std::string md_escape(const std::string& in) {
  std::string o;
  for (const char c : in) {
    if (c == '\n') {
      o += ' ';
      continue;
    }
    if (std::string_view("\\`*_[]<>|#").find(c) != std::string_view::npos) o += '\\';
    o += c;
  }
  return o;
}

std::string html_para(const Para& p) {
  std::string o;
  for (const auto& r : p)
    o += r.src.empty() ? html_escape(r.text) : "<span class=\"v\" data-src=\"" + html_escape(r.src) + "\" title=\"" + html_escape(r.src) + "\">" + html_escape(r.text) + "</span>";
  return o;
}

constexpr const char* kCss = R"(
:root{--bg:#fbfaf8;--fg:#1d1f21;--muted:#6b6f76;--line:#e3e1dc;--card:#ffffff;--warn-bg:#fff4e0;--warn-fg:#7a4a00;--ok:#1f7a4d;--accent:#2f5d9e}
@media (prefers-color-scheme: dark){:root{--bg:#16181b;--fg:#e6e6e3;--muted:#9a9ea5;--line:#2c3036;--card:#1d2024;--warn-bg:#3a2d12;--warn-fg:#f4c97a;--ok:#6fcf9f;--accent:#8fb3ec}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 16px 64px}
h1{font-size:24px;margin:0 0 4px}
h2{font-size:18px;margin:32px 0 8px;padding-bottom:4px;border-bottom:1px solid var(--line)}
nav{display:flex;flex-wrap:wrap;gap:4px 16px;margin:8px 0 16px;font-size:14px}
nav a{color:var(--accent);text-decoration:none}
p{margin:8px 0}
.warn{background:var(--warn-bg);color:var(--warn-fg);padding:8px 12px;border-radius:6px}
.scroll{overflow-x:auto;margin:8px 0}
table{border-collapse:collapse;background:var(--card);font-size:14px;min-width:40%}
th,td{border:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}
th{font-weight:600}
caption{caption-side:bottom;text-align:left;color:var(--muted);font-size:12px;padding-top:2px}
code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px}
.v{border-bottom:1px dotted var(--muted)}
caption,.srcnote{display:none}
.src caption,.src .srcnote{display:block}
.src .v::after{content:" " attr(data-src);font:11px ui-monospace,monospace;color:var(--muted)}
footer{margin-top:40px;color:var(--muted);font-size:13px}
label{font-size:14px;color:var(--muted)}
)";

std::string write_html(const json& summary, const std::vector<Section>& secs, const std::string& json_name) {
  Src src{summary};
  const std::string board = src.has("/run/board") ? value_text(src.at("/run/board")) : std::string("board");
  std::string o = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>Run report: " +
                  html_escape(board) + "</title>\n<style>" + kCss + "</style></head>\n<body><main>\n";
  o += "<h1>Run report: " + (src.has("/run/board") ? html_para({src.v("/run/board")}) : html_escape(board)) + "</h1>\n";
  o += "<label><input type=\"checkbox\" onchange=\"document.body.classList.toggle('src',this.checked)\"> Show the JSON field of every value</label>\n<nav>";
  for (const auto& s : secs)
    if (!s.title.empty()) o += "<a href=\"#" + s.id + "\">" + html_escape(s.title) + "</a>";
  o += "</nav>\n";
  for (const auto& s : secs) {
    o += "<section id=\"" + s.id + "\">";
    if (!s.title.empty()) o += "<h2>" + html_escape(s.title) + "</h2>";
    o += "\n";
    for (const auto& b : s.blocks) {
      if (b.kind == Block::Text) o += "<p>" + html_para(b.para) + "</p>\n";
      else if (b.kind == Block::Warn) o += "<p class=\"warn\">" + html_para(b.para) + "</p>\n";
      else {
        o += "<div class=\"scroll\"><table>";
        if (!b.table.fields.empty()) o += "<caption><code>" + html_escape(b.table.fields) + "</code></caption>";
        o += "<thead><tr>";
        for (const auto& h : b.table.head) o += "<th>" + html_escape(h) + "</th>";
        o += "</tr></thead><tbody>\n";
        for (const auto& r : b.table.rows) {
          o += "<tr>";
          for (const auto& c : r) o += "<td>" + html_para(c) + "</td>";
          o += "</tr>\n";
        }
        o += "</tbody></table></div>\n";
      }
    }
    o += "</section>\n";
  }
  o += "<footer>Every number and name in this report is a field of <code>" + html_escape(json_name) +
       "</code>. Hover over a value, or tick the box at the top, to see its JSON pointer.</footer>\n</main></body></html>\n";
  return o;
}

std::string write_markdown(const json& summary, const std::vector<Section>& secs, const std::string& json_name) {
  Src src{summary};
  std::vector<std::string> notes;  // footnote n+1 -> JSON pointer of a value in running text
  auto para = [&](const Para& p, bool footnotes) {
    std::string o;
    for (const auto& r : p) {
      o += md_escape(r.text);
      if (!r.src.empty() && footnotes) {
        notes.push_back(r.src);
        o += "[^" + std::to_string(notes.size()) + "]";
      }
    }
    return o;
  };
  std::string o = "# Run report: " + (src.has("/run/board") ? para({src.v("/run/board")}, true) : std::string("board")) + "\n\n";
  for (const auto& s : secs) {
    if (!s.title.empty()) o += "## " + s.title + "\n\n";
    for (const auto& b : s.blocks) {
      if (b.kind == Block::Text) o += para(b.para, true) + "\n\n";
      else if (b.kind == Block::Warn) o += "> **" + para(b.para, true) + "**\n\n";
      else {
        // A key/value table gets the JSON pointer of each value in a column of its own; others name their rows' source.
        const bool keyed = b.table.fields.empty();
        o += "|";
        for (const auto& h : b.table.head) o += " " + md_escape(h) + " |";
        if (keyed) o += " JSON |";
        o += "\n|";
        for (std::size_t i = 0; i < b.table.head.size() + (keyed ? 1 : 0); ++i) o += " --- |";
        o += "\n";
        for (const auto& r : b.table.rows) {
          o += "|";
          for (const auto& c : r) o += " " + para(c, false) + " |";
          if (keyed) o += " `" + (r.size() > 1 && !r[1].empty() ? r[1].front().src : std::string()) + "` |";
          o += "\n";
        }
        if (!keyed) o += "\nRows: `" + b.table.fields + "`\n";
        o += "\n";
      }
    }
  }
  o += "---\n\nEvery number and name in this report is a field of `" + json_name + "`; the notes give the JSON pointer of each value in the text.\n\n";
  for (std::size_t i = 0; i < notes.size(); ++i) o += "[^" + std::to_string(i + 1) + "]: `" + notes[i] + "`\n";
  return o;
}

}  // namespace

Format format_for_path(const std::string& path) {
  auto ends = [&](const char* e) {
    const std::string x(e);
    return path.size() >= x.size() && path.compare(path.size() - x.size(), x.size(), x) == 0;
  };
  return ends(".md") || ends(".markdown") ? Format::Markdown : Format::Html;
}

std::string value_text(const json& v) {
  if (v.is_null()) return "n/a";
  if (v.is_boolean()) return v.get<bool>() ? "yes" : "no";
  if (v.is_number_integer() || v.is_number_unsigned()) return v.dump();
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!std::isfinite(d)) return "n/a";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", d);
    std::string s(buf);
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
  }
  if (v.is_string()) return v.get<std::string>();
  return v.dump();
}

std::string render(const json& summary, Format format, const std::string& json_name) {
  const auto secs = Builder(summary).build();
  return format == Format::Html ? write_html(summary, secs, json_name) : write_markdown(summary, secs, json_name);
}

}  // namespace tmk::report
