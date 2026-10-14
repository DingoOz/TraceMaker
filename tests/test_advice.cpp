// SPDX-License-Identifier: GPL-3.0-or-later
// Advice (doc 17, issue #26): the advice file is read strictly and checked against the board, its regions steer the router, advised
// variants can only make the result better, the stuck report says what failed and can be redacted, and a broken advisor is a warning.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "advice/advice.hpp"
#include "advice/advisor.hpp"
#include "advice/stuck_report.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

// Two nets on a 20 x 20 mm, two-layer board: A runs along y = 0 between x = -8 and 8, B along x = 0 between y = -6 and 6, so B has to
// cross A (a via and the other layer) in the middle of the board unless something keeps it out of there. A third net C sits in a corner.
Loaded load_cross() {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.12;
  p["bounds"] = {{"minX", -10.0}, {"maxX", 10.0}, {"minY", -10.0}, {"maxY", 10.0}};
  json obstacles = json::array(), connections = json::array();
  auto pad = [&](const std::string& id, const std::string& part, double x, double y) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", 0.6}, {"height", 0.5},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", part}});
  };
  auto conn = [&](const std::string& name, const std::string& a, double ax, double ay, const std::string& b, double bx, double by) {
    connections.push_back({{"name", name},
                           {"pointsToConnect", {{{"x", ax}, {"y", ay}, {"layer", "top"}, {"pointId", a}}, {{"x", bx}, {"y", by}, {"layer", "top"}, {"pointId", b}}}},
                           {"rootConnectionName", name}});
  };
  pad("a1", "U1", -8, 0);
  pad("a2", "U2", 8, 0);
  pad("b1", "U3", 0, -6);
  pad("b2", "U4", 0, 6);
  pad("c1", "U5", -8, 8);
  pad("c2", "U6", -4, 8);
  conn("NET_A", "a1", -8, 0, "a2", 8, 0);
  conn("NET_B", "b1", 0, -6, "b2", 0, 6);
  conn("NET_C", "c1", -8, 8, "c2", -4, 8);
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_advice_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

std::string net_of(const model::Board& b, const std::string& ref) {
  for (const auto& p : b.pads)
    if (p.footprint >= 0 && b.footprints[static_cast<std::size_t>(p.footprint)].reference == ref) return b.nets[static_cast<std::size_t>(p.net)].name;
  return {};
}

// Distance from the centre line of a track to an axis-aligned rectangle (mm); 0 when they overlap.
double track_rect_distance_mm(const model::Track& t, double x0, double y0, double x1, double y1) {
  double best = 1e9;
  const int steps = 200;
  for (int i = 0; i <= steps; ++i) {
    const double f = static_cast<double>(i) / steps;
    const double x = nm_to_mm(t.a.x) + f * nm_to_mm(t.b.x - t.a.x), y = nm_to_mm(t.a.y) + f * nm_to_mm(t.b.y - t.a.y);
    const double dx = std::max({x0 - x, 0.0, x - x1}), dy = std::max({y0 - y, 0.0, y - y1});
    best = std::min(best, std::hypot(dx, dy));
  }
  return best;
}

route::RouterOptions base_options() {
  route::RouterOptions o;
  o.work_budget = 400'000;
  o.gpu_device = -1;
  o.optimize = false;
  return o;
}

}  // namespace

TEST_CASE("advice: a full file is read, and nothing it does not understand is", "[advice]") {
  const std::string good = R"({"version": 1, "note": "hello", "variants": 3,
    "first_nets": ["NET_A"], "priority": [["U1.1", "U2.1"], {"a": "U3.1", "b": "U4.1"}],
    "layers": {"no_tracks": ["B.Cu"], "cost": {"F.Cu": 2.5}},
    "regions": [{"name": "r", "rect": [10, 2, 0, 1], "layers": ["F.Cu"], "reserve_for": ["NET_A"], "vias": false}]})";
  const auto a = advice::parse_advice(good, "t.json");
  CHECK(a.variants == 3);
  CHECK(a.first_nets == std::vector<std::string>{"NET_A"});
  REQUIRE(a.priority.size() == 2);
  CHECK(a.priority[1].second == "U4.1");
  CHECK(a.no_tracks == std::vector<std::string>{"B.Cu"});
  REQUIRE(a.regions.size() == 1);
  CHECK(a.regions[0].polygon.size() == 4);
  CHECK_FALSE(a.regions[0].vias);
  CHECK(a.regions[0].tracks);
  // The corners are normalised: the rectangle was given right-to-left and bottom-to-top.
  CHECK(a.regions[0].polygon[0].x == 0);
  CHECK(a.regions[0].polygon[2].x == 10'000'000);

  auto refused = [](const std::string& text, const std::string& needle) {
    try {
      advice::parse_advice(text, "bad.json");
    } catch (const std::runtime_error& e) {
      const std::string m = e.what();
      INFO(m);
      CHECK(m.find("bad.json") != std::string::npos);
      CHECK(m.find(needle) != std::string::npos);
      return;
    }
    FAIL("accepted: " + text);
  };
  refused("[]", "must be a JSON map");
  refused(R"({"version": 2})", "version");
  refused(R"({})", "version");
  refused(R"({"version": 1, "frist_nets": []})", "unknown key 'frist_nets'");
  refused(R"({"version": 1, "first_nets": "NET_A"})", "first_nets");
  refused(R"({"version": 1, "first_nets": ["A", "A"]})", "listed twice");
  refused(R"({"version": 1, "first_nets": [""]})", "must not be empty");
  refused(R"({"version": 1, "variants": 99})", "variants");
  refused(R"({"version": 1, "layers": {"cost": {"F.Cu": 0.2}}})", "layers.cost.F.Cu");
  refused(R"({"version": 1, "layers": {"nope": 1}})", "unknown key 'nope'");
  refused(R"({"version": 1, "priority": [["U1.1"]]})", "priority[0]");
  refused(R"({"version": 1, "priority": [["U1.1", "U1.1"]]})", "same pad twice");
  refused(R"({"version": 1, "regions": [{"rect": [0,0,1,1]}]})", "reserve_for");
  refused(R"({"version": 1, "regions": [{"rect": [0,0,1,1], "reserve_for": ["A"], "avoid_nets": ["B"]}]})", "exactly one");
  refused(R"({"version": 1, "regions": [{"rect": [0,0,0,1], "reserve_for": ["A"]}]})", "no area");
  refused(R"({"version": 1, "regions": [{"polygon": [[0,0],[1,1]], "reserve_for": ["A"]}]})", "polygon");
  refused(R"({"version": 1, "regions": [{"rect": [0,0,1,1], "reserve_for": ["A"], "tracks": false, "vias": false}]})", "neither");
  refused(R"({"version": 1, "regions": [{"rect": [0,0,1e9,1], "reserve_for": ["A"]}]})", "between");
  refused("{\"version\": 1,\n \"first_nets\": [,]}", "line 2");  // a syntax error says where
  std::string many = R"({"version": 1, "first_nets": [)";
  for (int i = 0; i < 65; ++i) many += std::string(i ? "," : "") + "\"N" + std::to_string(i) + "\"";
  refused(many + "]}", "more than 64");
}

TEST_CASE("advice: names are checked against the board, with hints", "[advice]") {
  const auto l = load_cross();
  auto problem = [&](const std::string& text) -> std::string {
    try {
      advice::check_advice_refs(advice::parse_advice(text, "x.json"), l.board);
    } catch (const std::runtime_error& e) {
      return e.what();
    }
    return {};
  };
  CHECK(problem(R"({"version": 1, "first_nets": ["NET_A"]})").empty());
  CHECK(problem(R"({"version": 1, "priority": [["U1.1", "U2.1"]]})").empty());
  const auto m = problem(R"({"version": 1, "first_nets": ["NET_Z"], "layers": {"no_tracks": ["Z.Cu"]}, "regions": [{"rect": [0,0,1,1], "reserve_for": ["NET_AA"]}]})");
  CHECK(m.find("3 problems") != std::string::npos);
  CHECK(m.find("no net named 'NET_Z'") != std::string::npos);
  CHECK(m.find("did you mean NET_A") != std::string::npos);
  CHECK(m.find("no copper layer named 'Z.Cu'") != std::string::npos);
  CHECK(m.find("the board has: F.Cu, B.Cu") != std::string::npos);
  CHECK(problem(R"({"version": 1, "priority": [["U1.1", "U9.1"]]})").find("no pad 'U9.1'") != std::string::npos);
  CHECK(problem(R"({"version": 1, "priority": [["U1.1", "U3.1"]]})").find("different nets") != std::string::npos);
  CHECK(problem(R"({"version": 1, "layers": {"no_tracks": ["F.Cu", "B.Cu"]}})").find("every copper layer") != std::string::npos);
  // Warnings, not errors: a region outside the board does nothing.
  std::vector<std::string> warnings;
  advice::check_advice_refs(advice::parse_advice(R"({"version": 1, "regions": [{"rect": [100,100,101,101], "reserve_for": ["NET_A"]}]})", "w.json"), l.board, &warnings);
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("outside the board") != std::string::npos);
}

TEST_CASE("advice: hints become router options; regions become a rule area and disallow rules", "[advice]") {
  const auto l = load_cross();
  const auto a = advice::parse_advice(R"({"version": 1, "first_nets": ["NET_B"], "priority": [["U1.1", "U2.1"]],
    "layers": {"no_tracks": ["B.Cu"], "cost": {"F.Cu": 2.5}},
    "regions": [{"rect": [-3, -2, 3, 2], "layers": ["F.Cu"], "reserve_for": ["NET_A"]},
                {"rect": [5, 5, 6, 6], "avoid_nets": ["NET_B", "NET_C"], "tracks": true, "vias": false}]})",
                                      "t.json");
  const auto ap = advice::apply_advice(a, l.board);
  const auto& s = ap.set;
  CHECK(s.first_nets == std::vector<std::string>{"NET_B"});
  CHECK(s.priority.size() == 1);
  CHECK(s.no_track_layers == model::layer_bit(1));
  REQUIRE(s.layer_cost_pm.size() == 2);
  CHECK(s.layer_cost_pm[0] == 2500);
  CHECK(s.layer_cost_pm[1] == 1000);
  REQUIRE(s.zones.size() == 2);
  CHECK(s.zones[0].rule_area);
  CHECK_FALSE(s.zones[0].keepout_tracks);  // a named place only; the rules below do the keeping out
  CHECK(s.zones[0].copper == model::layer_bit(0));
  // Region 1: one rule, for the layer it names, "every net but NET_A". Region 2: one rule per net, on every layer.
  REQUIRE(s.rules.size() == 3);
  CHECK(s.rules[0].layer == "F.Cu");
  CHECK(s.rules[0].condition == "A.intersectsArea('tmk:advice:1') && A.NetName != 'NET_A'");
  CHECK(s.rules[0].constraints.at(0).type == "disallow");
  CHECK(s.rules[0].constraints.at(0).items == std::vector<std::string>{"track", "via"});
  CHECK(s.rules[1].condition == "A.intersectsArea('tmk:advice:2') && A.NetName == 'NET_B'");
  CHECK(s.rules[1].layer.empty());
  CHECK(s.rules[1].constraints.at(0).items == std::vector<std::string>{"track"});
  CHECK(ap.report["hints"].size() == 6);
  CHECK(s.active());
  CHECK_FALSE(route::RouterOptions::AdviceSet{}.active());
  // The saved form reads back to the same advice.
  const auto again = advice::parse_advice(advice::advice_to_json(a).dump(), "again.json");
  CHECK(again.first_nets == a.first_nets);
  CHECK(again.priority == a.priority);
  CHECK(again.regions.size() == 2);
  CHECK(again.regions[0].polygon == a.regions[0].polygon);
  CHECK(again.regions[1].avoid_nets == a.regions[1].avoid_nets);
  CHECK(advice::advice_schema().contains("regions"));
}

TEST_CASE("advice: a region reserved for one net keeps the others out, and the others route around it", "[advice][route]") {
  const auto l = load_cross();
  const std::string na = net_of(l.board, "U1"), nb = net_of(l.board, "U3");
  REQUIRE(na == "NET_A");
  REQUIRE(nb == "NET_B");
  auto opt = base_options();
  const auto plain = route::Router(l.board, l.rules, opt).run();
  REQUIRE(plain.routed == plain.connections);
  // Without the region, B runs straight through the middle.
  const auto nb_id = l.board.net_by_name(nb);
  auto through = [&](const route::RouteResult& r) {
    double d = 1e9;
    for (const auto& t : r.tracks)
      if (t.net == nb_id) d = std::min(d, track_rect_distance_mm(t, -3, -2, 3, 2));
    return d;
  };
  CHECK(through(plain) < 0.05);
  // With it, the board file is unchanged and B keeps clear of the region; A (the reserved net) still crosses it.
  const auto ap = advice::apply_advice(advice::parse_advice(R"({"version": 1, "regions": [{"rect": [-3, -2, 3, 2], "reserve_for": ["NET_A"]}]})", "r.json"), l.board);
  opt.extra_zones = ap.set.zones;
  opt.extra_rules = ap.set.rules;
  const auto guided = route::Router(l.board, l.rules, opt).run();
  REQUIRE(guided.routed == guided.connections);
  CHECK(through(guided) >= 0.05);
  double a_in = 1e9;
  for (const auto& t : guided.tracks)
    if (t.net == l.board.net_by_name(na)) a_in = std::min(a_in, track_rect_distance_mm(t, -3, -2, 3, 2));
  CHECK(a_in < 0.05);
  // The caller's rules were not touched: the extra rules live in the router's copy.
  CHECK(l.rules.custom.empty());
}

TEST_CASE("advice: the portfolio adds advised variants after the others and never does worse", "[advice][route]") {
  const auto l = load_cross();
  auto opt = base_options();
  const auto plain = route::route_portfolio(l.board, l.rules, opt, 2, {}, 2);
  REQUIRE(plain.indices.size() == 2);
  CHECK(plain.indices == std::vector<int>{0, 1});
  const auto ap = advice::apply_advice(advice::parse_advice(R"({"version": 1, "variants": 2, "first_nets": ["NET_B"],
      "regions": [{"rect": [-3, -2, 3, 2], "reserve_for": ["NET_A"]}]})", "p.json"), l.board);
  opt.advice = ap.set;
  const auto advised = route::route_portfolio(l.board, l.rules, opt, 2, {}, 2);
  REQUIRE(advised.indices.size() == 4);
  CHECK(advised.indices == std::vector<int>{0, 1, route::kAdvisedIndex, route::kAdvisedIndex + 1});
  CHECK(advised.variants[2].find("+ advice") != std::string::npos);
  CHECK(advised.best.routed >= plain.best.routed);
  // The unadvised variants are the ones they were: advice does not change them.
  CHECK(advised.routed[0] == plain.routed[0]);
  CHECK(advised.routed[1] == plain.routed[1]);
  // Same advice, same work budget: the same copper for any thread count (N3).
  const auto one = route::route_portfolio(l.board, l.rules, opt, 2, {}, 1);
  const auto four = route::route_portfolio(l.board, l.rules, opt, 2, {}, 4);
  REQUIRE(one.best.tracks.size() == four.best.tracks.size());
  for (std::size_t i = 0; i < one.best.tracks.size(); ++i) {
    CHECK(one.best.tracks[i].a == four.best.tracks[i].a);
    CHECK(one.best.tracks[i].b == four.best.tracks[i].b);
    CHECK(one.best.tracks[i].layer == four.best.tracks[i].layer);
  }
  CHECK(one.best_variant == four.best_variant);
}

TEST_CASE("advice: the order of results is the portfolio's, and a tie is not better", "[advice][route]") {
  route::RouteResult a, b;
  a.routed = 5, b.routed = 4;
  CHECK(route::route_result_better(a, b));
  CHECK_FALSE(route::route_result_better(b, a));
  b.routed = 5;
  CHECK_FALSE(route::route_result_better(a, b));
  CHECK_FALSE(route::route_result_better(b, a));
  b.vias.resize(1);
  CHECK(route::route_result_better(a, b));
}

TEST_CASE("stuck report: names what failed, where, and why; redaction hides the board's names", "[advice][route]") {
  const auto l = load_cross();
  auto opt = base_options();
  opt.work_budget = 1;  // out of budget at once: everything stays unrouted
  const auto res = route::Router(l.board, l.rules, opt).run();
  REQUIRE(res.unrouted.size() >= 2);  // one search may run before the budget is seen to be used up
  CHECK(std::any_of(res.unrouted.begin(), res.unrouted.end(), [](const auto& u) { return u.reason == "not_attempted"; }));
  CHECK(res.unrouted[0].pad_a >= 0);
  advice::StuckInput in;
  in.board = &l.board;
  in.rules = &l.rules;
  in.result = &res;
  in.board_name = "secret_board.kicad_pcb";
  const auto rep = advice::build_stuck_report(in);
  const auto& j = rep.json;
  CHECK(j["tracemaker_stuck_report"] == 1);
  CHECK(j["result"]["unrouted"] == res.unrouted.size());
  REQUIRE(j["unrouted"].size() == res.unrouted.size());
  const auto& u = j["unrouted"][0];
  CHECK(u["from"].contains("x_mm"));
  CHECK(u["from"]["layers"][0] == "F.Cu");
  CHECK(u.contains("region_mm"));
  CHECK(u["reason"] == res.unrouted[0].reason);
  CHECK(j["board"]["name"] == "secret_board.kicad_pcb");
  CHECK(j["disclosure"]["redacted"] == false);
  CHECK(j.contains("advice_schema"));
  CHECK(j.contains("escape_analysis"));  // an empty list here: three two-pin nets have no dense package
  CHECK(rep.aliases.empty());
  const std::string plain = j.dump();
  CHECK(plain.find("NET_A") != std::string::npos);
  // Redacted: no name of the board's survives, and the aliases map back.
  in.redact = true;
  const auto red = advice::build_stuck_report(in);
  const std::string text = red.json.dump();
  CHECK(text.find("NET_") == std::string::npos);
  CHECK(text.find("secret_board") == std::string::npos);
  CHECK(text.find("U1") == std::string::npos);
  CHECK(text.find("library_id") == std::string::npos);
  CHECK(red.json["disclosure"]["redacted"] == true);
  REQUIRE_FALSE(red.aliases.empty());
  CHECK(red.aliases.net.at("N1").rfind("NET_", 0) == 0);
  // Advice written with the aliases reads back with the board's names.
  auto adv = advice::parse_advice(R"({"version": 1, "first_nets": ["N1"], "priority": [["P1.1", "P2.1"]]})", "alias.json");
  advice::unalias(adv, red.aliases);
  CHECK(adv.first_nets[0].rfind("NET_", 0) == 0);
  CHECK(adv.priority[0].first.rfind("U", 0) == 0);
  CHECK(adv.priority[0].first.substr(adv.priority[0].first.find('.')) == ".1");
}

TEST_CASE("advisor: a process is run with a time limit; whatever it does, the answer is checked", "[advice]") {
  auto r = advice::run_process("cat", "hello", 5);
  CHECK(r.started);
  CHECK(r.exit_code == 0);
  CHECK(r.out == "hello");
  r = advice::run_process("echo oops >&2; exit 3", "", 5);
  CHECK(r.exit_code == 3);
  CHECK(r.err == "oops\n");
  // It never reads its input: writing a lot must neither block nor kill us.
  r = advice::run_process("true", std::string(1 << 20, 'x'), 5);
  CHECK(r.exit_code == 0);
  CHECK_FALSE(r.timed_out);
  // Too slow: the whole process group is killed.
  r = advice::run_process("sleep 30; echo late", "", 0.3);
  CHECK(r.timed_out);
  CHECK(r.seconds < 5);
  // More output than the limit: truncated and flagged, and the child is not left blocked on a full pipe.
  r = advice::run_process("head -c 3000000 /dev/zero", "", 10, 1000);
  CHECK(r.out.size() == 1000);
  CHECK(r.out_truncated);
  CHECK(r.exit_code == 0);
  r = advice::run_process("exit 127", "", 5);
  CHECK(r.exit_code == 127);
#ifdef __linux__
  // The advisor gets the default SIGPIPE (an ignored signal would stay ignored across exec): bit 12 of SigIgn is clear.
  r = advice::run_process("grep SigIgn /proc/$$/status", "", 10);
  REQUIRE(r.out.find("SigIgn:") == 0);
  const auto ignored = std::stoull(r.out.substr(r.out.find_first_of("0123456789abcdef", 7)), nullptr, 16);
  CHECK((ignored & (1ull << 12)) == 0);
#endif

  const json report = {{"tracemaker_stuck_report", 1}};
  advice::AliasMap none;
  auto ask = [&](const std::string& cmd) { return advice::consult_advisor({cmd, 5}, report, none, "answer"); };
  auto ok = ask(R"(cat >/dev/null; echo '{"version": 1, "first_nets": ["NET_A"]}')");
  CHECK(ok.ok);
  CHECK(ok.advice.first_nets.at(0) == "NET_A");
  CHECK(ok.summary["program"] == "cat");  // only the program name is kept, never the command line
  CHECK(ok.summary["ok"] == true);
  CHECK_FALSE(ask("exit 2").ok);
  CHECK(ask("exit 2").error.find("status 2") != std::string::npos);
  CHECK(ask("echo not json").error.find("refused") != std::string::npos);
  CHECK(ask(R"(echo '{"version": 1, "rm_rf": true}')").error.find("unknown key") != std::string::npos);
  CHECK(ask("no_such_program_anywhere 2>/dev/null").error.find("status 127") != std::string::npos);
  const auto slow = advice::consult_advisor({"sleep 30", 0.3}, report, none, "answer");
  CHECK_FALSE(slow.ok);
  CHECK(slow.error.find("did not answer") != std::string::npos);
  // The advisor's command line may hold a key: it is not echoed anywhere.
  const auto secret = advice::consult_advisor({"cat >/dev/null; echo '{\"version\": 1}' #sk-secret-key", 5}, report, none, "answer");
  CHECK(secret.summary.dump().find("sk-secret-key") == std::string::npos);
}
