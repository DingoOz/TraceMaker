// SPDX-License-Identifier: GPL-3.0-or-later
// The run report (doc 18 D3, D136): every number and name in the report is a field of the summary, and the report's own
// sections (rules, DRC before and after) say what they should.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <regex>
#include <string>

#include <nlohmann/json.hpp>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "report/render.hpp"
#include "report/run_report.hpp"

using namespace tmk;
using nlohmann::json;

namespace {

const std::string kBoard = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";

// A summary with every section the report renders, values chosen so that a mix-up between fields shows.
json full_summary() {
  return json::parse(R"J({
    "routed": 41, "connections": 44, "open": 3, "completion_pct": 93.18, "tracks": 212, "vias": 17, "seconds": 12.5,
    "variant": 3, "variant_name": "hard-first", "time_limited": true, "pitch_mm": 0.127, "unconnected_after_refill": 2,
    "work_all_variants": {"total": {"expansions": 987654}},
    "failures": ["SDA: U1.4 -> R7.1 (no legal path)"],
    "variants": [{"variant": 0, "name": "base", "routed": 40, "seconds": 5.25}, {"variant": 3, "name": "hard-first", "routed": 41, "seconds": 6.5}],
    "run": {"version": "0.10.0", "board": "demo-2.kicad_pcb", "input": "in/demo-2.kicad_pcb", "output": "out/demo-2.kicad_pcb", "date": "2026-10-24T01:02:03Z",
            "seed": 7, "work_budget": 3000000, "time_limit_s": 120, "variants": 8, "threads": 4,
            "options": [{"name": "--work", "value": "3000000"}, {"name": "--soft-zones", "value": "on"}]},
    "rules": {"files": {"project": "demo-2.kicad_pro", "project_found": true, "custom_rules": "demo-2.kicad_dru", "custom_rules_found": false},
              "minimums": {"clearance_mm": 0.15, "track_width_mm": 0.2, "via_diameter_mm": 0.45, "via_annular_width_mm": 0.1, "drill_mm": 0.3,
                           "hole_clearance_mm": 0.25, "hole_to_hole_mm": 0.5, "copper_edge_clearance_mm": 0.4, "blind_buried_vias": false, "microvias": false},
              "net_classes": [{"name": "Default", "nets": 30, "clearance_mm": 0.2, "track_width_mm": 0.25, "via_diameter_mm": 0.6, "via_drill_mm": 0.3,
                               "diff_pair_width_mm": 0.2, "diff_pair_gap_mm": 0.25}],
              "custom_rules": [{"name": "rule 9", "layer": "", "condition": "A.NetClass == 'HV'", "constraints": ["clearance"]}], "custom_rule_count": 1,
              "not_read": [{"source": "custom rules", "text": "rule 'x2': cannot read value of min"}], "not_read_count": 1},
    "stuck": {"result": {"cuts": {"unroutable": [{"axis": "x", "at_mm": 21.5, "demand": 9, "capacity": 6}]}},
              "unrouted": [{"net": "SDA", "from": {"part": "U1", "pad": "4"}, "to": {"part": "R7", "pad": "1"}, "length_mm": 13.2, "reason": "no_legal_path",
                            "why": "no path within 0.15 mm clearance", "blockers": ["SCL", "+3V3"]},
                           {"net": "GND", "from": {"part": "C5", "pad": "2"}, "to": {"zone": true}, "length_mm": 0, "reason": "boxed_in", "why": "boxed in", "blockers": []}],
              "unrouted_not_shown": 1,
              "unrouted_by_reason": [{"name": "no_legal_path", "count": 2}, {"name": "boxed_in", "count": 1}],
              "unrouted_by_part": [{"name": "U1", "count": 2}],
              "escape_analysis": [{"part": "U9", "pitch_mm": 0.4, "pins": 64, "escapable": 61, "cannot_escape": 3, "hint": "use 0.1 mm tracks",
                                   "dead": [{"pads": ["B2", "C3", "D4"], "count": 3, "cause": "trapped on every layer it can reach"}]},
                                  {"part": "U2", "pitch_mm": 0.5, "pins": 17, "escapable": 17, "cannot_escape": 0, "dead": []}]},
    "critical_nets": {"totals": {"nets": 2, "vias": 5, "void_crossings": 1, "missing_stitches": 1, "missing_stitches_before_stitching": 3},
                      "nets": [{"name": "CLK", "tier": 1, "why": "clock", "vias": 3, "length_mm": 22.75, "unrouted": 0, "void_crossings": 1, "missing_stitches": 1,
                                "warnings": ["runs over 1 void", "layer change F.Cu to B.Cu at (12.480, 5.980) mm: no via of GND within 5.0 mm"]}]},
    "pairs": [{"net_a": "USB_P", "net_b": "USB_N", "length_mm": 18.5, "coupled_share": 0.86, "gap_mm": 0.2, "target_gap_mm": 0.18, "skew_mm": 0.031}],
    "drc": {"checker": "tracemaker drc", "zones_refilled": true,
            "before": {"errors": 4, "warnings": 6, "unconnected": 44, "notes": []}, "after": {"errors": 5, "warnings": 8, "unconnected": 2, "notes": ["zone 3 has no outline"],
                      "unconnected_items": [{"what": "Missing connection in net GND", "from": "Pad 2 [GND] of C5", "to": "Zone [GND] on B.Cu", "x_mm": 10.5, "y_mm": 20.25}],
                      "unconnected_items_not_shown": 1},
            "types": [{"type": "clearance", "severity": "error", "routing": true, "before": 4, "after": 5, "added": 1}],
            "added_routing_errors": 1, "removed": 0, "unconnected_change": -42}
  })J");
}

std::string unescape(std::string s) {
  for (const auto& [from, to] : {std::pair<const char*, const char*>{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&amp;", "&"}}) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + 1)) s.replace(at, std::string(from).size(), to);
  }
  return s;
}

}  // namespace

TEST_CASE("report: value text", "[report]") {
  CHECK(report::value_text(json(41)) == "41");
  CHECK(report::value_text(json(0.127)) == "0.127");
  CHECK(report::value_text(json(12.5)) == "12.5");
  CHECK(report::value_text(json(3.0)) == "3");
  CHECK(report::value_text(json(-0.0001)) == "0");
  CHECK(report::value_text(json(true)) == "yes");
  CHECK(report::value_text(json(nullptr)) == "n/a");
  CHECK(report::value_text(json("U1")) == "U1");
  CHECK(report::format_for_path("r.md") == report::Format::Markdown);
  CHECK(report::format_for_path("r.html") == report::Format::Html);
}

TEST_CASE("report: every value in the HTML is the field its pointer names, and no number is without one", "[report]") {
  const json s = full_summary();
  const std::string html = report::render(s, report::Format::Html, "summary.json");
  const std::regex span(R"re(<span class="v" data-src="([^"]*)" title="[^"]*">([^<]*)</span>)re");
  int values = 0;
  for (auto it = std::sregex_iterator(html.begin(), html.end(), span); it != std::sregex_iterator(); ++it) {
    const std::string ptr = unescape((*it)[1]);
    INFO(ptr);
    REQUIRE(s.contains(json::json_pointer(ptr)));
    CHECK(unescape((*it)[2]) == report::value_text(s.at(json::json_pointer(ptr))));
    ++values;
  }
  CHECK(values > 100);
  // What is left without the values, the code spans, the page's style and its title must hold no digit: the renderer's own
  // words never carry a number.
  std::string rest = std::regex_replace(html, std::regex(R"(<style>[\s\S]*?</style>|<title>[\s\S]*?</title>|<code>[\s\S]*?</code>)"), "");
  rest = std::regex_replace(rest, span, "");
  rest = std::regex_replace(rest, std::regex("<[^>]*>"), "");
  INFO(rest);
  CHECK(std::regex_search(rest, std::regex("[0-9]")) == false);
  // The sections a designer looks for are there, with what they must say.
  for (const char* id : {"id=\"result\"", "id=\"open\"", "id=\"rules\"", "id=\"si\"", "id=\"drc\"", "id=\"options\""}) CHECK(html.find(id) != std::string::npos);
  CHECK(html.find("Cut proof") != std::string::npos);
  CHECK(html.find("cannot leave their package") != std::string::npos);
  CHECK(html.find("data-src=\"/stuck/escape_analysis/0/dead/0/pads/2\"") != std::string::npos);  // the trapped pins are named
  CHECK(html.find("data-src=\"/stuck/escape_analysis/1/part\"") == std::string::npos);           // a package whose pins all escape is not
  CHECK(html.find("could not be read") != std::string::npos);
  CHECK(html.find("not the work budget, ended the search") != std::string::npos);
  CHECK(html.find("data-src=\"/drc/after/unconnected_items/0/to\"") != std::string::npos);
}

TEST_CASE("report: the Markdown names the JSON pointer of each value in its text", "[report]") {
  const json s = full_summary();
  const std::string md = report::render(s, report::Format::Markdown, "summary.json");
  const std::regex note(R"(\[\^\d+\]: `([^`]*)`)");
  int notes = 0;
  for (auto it = std::sregex_iterator(md.begin(), md.end(), note); it != std::sregex_iterator(); ++it, ++notes) CHECK(s.contains(json::json_pointer((*it)[1].str())));
  CHECK(notes > 10);
  CHECK(md.find("| Connections | 44 | `/connections` |") != std::string::npos);
  CHECK(md.find("Rows: `/stuck/unrouted/<row>`") != std::string::npos);
}

TEST_CASE("report: a plain summary renders, and says what it lacks", "[report]") {
  const json s = json::parse(R"({"routed": 5, "connections": 5, "tracks": 9, "vias": 0, "seconds": 0.5})");
  const std::string html = report::render(s, report::Format::Html, "s.json");
  CHECK(html.find("Every connection was routed") != std::string::npos);
  CHECK(html.find("no rules section") != std::string::npos);
  CHECK(html.find("No audit was asked for") != std::string::npos);
  // Every planned connection routed, but the refilled board leaves items unconnected: the report must list them, not say nothing is open.
  const json refill = json::parse(R"({"routed": 5, "connections": 5, "open": 0, "unconnected_after_refill": 1,
    "drc": {"after": {"unconnected": 1, "unconnected_items": [{"what": "Missing connection in net GND", "from": "Pad 1 [GND] of U1", "to": "Zone [GND] on F.Cu",
                                                               "x_mm": 1, "y_mm": 2}]}}})");
  const std::string h3 = report::render(refill, report::Format::Html, "s.json");
  CHECK(h3.find("Nothing is open") == std::string::npos);
  CHECK(h3.find("Every connection was routed") == std::string::npos);
  CHECK(h3.find("the board is not finished") != std::string::npos);
  CHECK(h3.find("data-src=\"/drc/after/unconnected_items/0/from\"") != std::string::npos);
  // Asked for, with nothing on the board to audit: the report must not say it was not asked for.
  const json asked = json::parse(R"({"routed": 5, "connections": 5, "run": {"options": [{"name": "--si-rules", "value": "true"}]}})");
  const std::string h2 = report::render(asked, report::Format::Html, "s.json");
  CHECK(h2.find("No audit was asked for") == std::string::npos);
  CHECK(h2.find("the board has nothing for them") != std::string::npos);
}

TEST_CASE("report: the rules section lists the project's classes and what was not read", "[report]") {
  const auto lb = io::read_board_file(kBoard);
  const auto rules = io::read_design_rules(kBoard);
  const auto j = report::rules_json(kBoard, lb.board, rules, {"rule 'r': cannot compile condition"});
  CHECK(j["files"]["project_found"] == true);
  CHECK(j["net_classes"].size() == rules.classes.size());
  int nets = 0;
  for (const auto& n : lb.board.nets) nets += n.id > 0 && !n.name.empty();
  int counted = 0;
  for (const auto& c : j["net_classes"]) counted += c["nets"].get<int>();
  CHECK(counted == nets);
  REQUIRE(j["not_read_count"].get<int>() >= 1);
  bool engine = false;
  for (const auto& w : j["not_read"]) engine = engine || (w["source"] == "custom rules" && w["text"] == "rule 'r': cannot compile condition");
  CHECK(engine);
}

TEST_CASE("report: DRC before and after counts what the new copper adds", "[report][drc]") {
  const auto lb = io::read_board_file(kBoard);
  const auto rules = io::read_design_rules(kBoard);
  const auto same = report::drc_json(lb.board, lb.board, rules, false);
  // Nothing is routed, so every unconnected item is listed after, each with its two ends.
  CHECK(same["after"]["unconnected_items"].size() == static_cast<std::size_t>(std::min(same["after"]["unconnected"].get<int>(), 300)));
  REQUIRE_FALSE(same["after"]["unconnected_items"].empty());
  CHECK(same["after"]["unconnected_items"][0].contains("from"));
  CHECK(same["added_routing_errors"] == 0);
  CHECK(same["removed"] == 0);
  CHECK(same["unconnected_change"] == 0);
  // A track of one net laid over a pad of another is a routing error the comparison must count.
  model::Board bad = lb.board;
  const model::Pad* a = nullptr;
  const model::Pad* b = nullptr;
  for (const auto& p : bad.pads) {
    if (p.net <= 0 || !(p.copper & model::layer_bit(0))) continue;
    if (!a) a = &p;
    else if (p.net != a->net) {
      b = &p;
      break;
    }
  }
  REQUIRE(a);
  REQUIRE(b);
  model::Track t;
  t.a = a->pos;
  t.b = b->pos;
  t.width = 200'000;
  t.layer = 0;
  t.net = a->net;
  bad.tracks.push_back(t);
  const auto worse = report::drc_json(lb.board, bad, rules, false);
  CHECK(worse["added_routing_errors"].get<int>() >= 1);
  CHECK(report::routing_violation("clearance"));
  CHECK_FALSE(report::routing_violation("silk_overlap"));
  CHECK_FALSE(report::routing_violation("unconnected_items"));
}
