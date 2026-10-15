// SPDX-License-Identifier: GPL-3.0-or-later
// The board session an agent drives (doc 19): the tools list and route the open connections, a route is kept only as a transaction, states can be undone and
// restored, the log replays to the same bytes, hints are read as an advice file, refused calls say why, `save` writes only to the file the session was started
// with, and both transports answer a bad request instead of failing.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"
#include "route/router.hpp"
#include "session/session.hpp"
#include "session/transport.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

// Three nets on a 20 x 20 mm, two-layer board: A along y = 0, B along x = 0 (it must cross A), C in a corner.
struct TestBoard {
  fs::path dir;
  std::string pcb;
  TestBoard() {
    static int counter = 0;
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
    dir = fs::temp_directory_path() / ("tm_session_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
    pcb = (dir / "b.kicad_pcb").string();
    std::ofstream(pcb) << files.pcb;
    std::ofstream(dir / "b.kicad_pro") << files.pro;
    if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  }
  ~TestBoard() { fs::remove_all(dir); }
  session::SessionOptions options() const {
    session::SessionOptions o;
    o.in = pcb;
    o.out = (dir / "out.kicad_pcb").string();
    o.threads = 2;
    return o;
  }
};

bool is_error(const json& j, const std::string& code) { return j.contains("error") && j["error"]["code"] == code; }

}  // namespace

TEST_CASE("session: the open connections are listed and a route closes them", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto info = s.call("board_info", json::object());
  CHECK(info["open_connections"] == 3);
  CHECK(info["nets"] == 3);
  CHECK(info["state"] == 0);
  const auto open = s.call("open_connections", json::object());
  REQUIRE(open["connections"].size() == 3);
  for (const auto& c : open["connections"]) {
    CHECK(c["net"].is_string());
    CHECK(c["a"].get<std::string>().find('.') != std::string::npos);
    CHECK(c["length_mm"].get<double>() > 1.0);
  }
  CHECK(s.call("open_connections", {{"net", "NET_B"}})["open"] == 1);

  const auto r = s.call("route", {{"work", 300000}});
  REQUIRE(r.contains("committed"));
  CHECK(r["committed"] == true);
  CHECK(r["routed"] == 3);
  CHECK(r["open_after"] == 0);
  CHECK(r["open_total"] == 0);
  CHECK(r["state"] == 1);
  CHECK_FALSE(r.contains("added_errors"));
  CHECK(r["work"]["used"].get<long>() > 0);
  CHECK(s.call("open_connections", json::object())["open"] == 0);
  const auto chk = s.call("check", json::object());
  CHECK(chk["unconnected"] == 0);
  CHECK(chk["added_errors"].empty());
  CHECK(chk["errors"].empty());
}

TEST_CASE("session: a route with one plain router is the router's own result (the reference path)", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto r = s.call("route", {{"work", 250000}, {"seed", 7}});
  REQUIRE(r["committed"] == true);
  auto lb = io::read_board_file(tb.pcb);
  const auto rules = io::read_design_rules(tb.pcb);
  route::RouterOptions o;
  o.seed = 7;
  o.work_budget = 250000;
  o.time_limit_s = 3600;
  o.gpu_device = -1;
  const auto direct = route::Router(lb.board, rules, o).run();
  CHECK(r["new_tracks"].get<std::size_t>() == direct.tracks.size());
  CHECK(r["new_vias"].get<std::size_t>() == direct.vias.size());
  CHECK(r["routed"].get<int>() == direct.routed);
  // The same copper, item by item: saving the session writes the router's tracks in the router's order.
  const auto saved = s.call("save", json::object());
  REQUIRE(saved["saved"] == true);
  const auto out = io::read_board_file(tb.options().out);
  REQUIRE(out.board.tracks.size() == direct.tracks.size());
  for (std::size_t i = 0; i < direct.tracks.size(); ++i) {
    CHECK(out.board.tracks[i].a.x == direct.tracks[i].a.x);
    CHECK(out.board.tracks[i].b.y == direct.tracks[i].b.y);
    CHECK(out.board.tracks[i].width == direct.tracks[i].width);
    CHECK(out.board.tracks[i].locked == false);  // session copper is locked inside the session only
  }
}

TEST_CASE("session: rip, undo and restore move between states", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  REQUIRE(s.call("route", {{"work", 300000}})["committed"] == true);
  CHECK(s.state() == 1);
  const auto rip = s.call("rip_net", {{"net", "NET_B"}});
  REQUIRE(rip.contains("state"));
  CHECK(rip["state"] == 2);
  CHECK(rip["removed_tracks"].get<int>() > 0);
  CHECK(s.call("open_connections", json::object())["open"] == 1);
  // A route of one net reports the board's total as well as its own open count.
  const auto again = s.call("route", {{"nets", {"NET_A"}}, {"work", 100000}, {"commit", false}});
  CHECK(again["open_total"] == 1);
  CHECK(is_error(s.call("rip_net", {{"net", "NET_B"}}), "not_allowed"));  // nothing of the session's left on it
  CHECK(s.call("undo", json::object())["id"] == 1);
  CHECK(s.call("open_connections", json::object())["open"] == 0);
  CHECK(s.call("restore", {{"state", 0}})["id"] == 0);
  CHECK(s.call("open_connections", json::object())["open"] == 3);
  CHECK(is_error(s.call("undo", json::object()), "not_allowed"));  // already at the input board
  CHECK(is_error(s.call("restore", {{"state", 9}}), "bad_argument"));
  // A change after a restore starts a new branch; the old states stay.
  REQUIRE(s.call("route", {{"nets", {"NET_C"}}, {"work", 100000}})["committed"] == true);
  const auto h = s.call("history", json::object());
  CHECK(h["states"].size() == 4);
  CHECK(h["states"][3]["parent"] == 0);
}

TEST_CASE("session: a dry run and a refused route change nothing", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto dry = s.call("route", {{"work", 300000}, {"commit", false}});
  CHECK(dry["committed"] == false);
  CHECK(dry["open_total"] == 3);  // the board's total, whatever nets the call named
  CHECK(dry["reason"] == "dry_run");
  CHECK(dry["routed"] == 3);
  CHECK(s.state() == 0);
  CHECK(s.call("open_connections", json::object())["open"] == 3);
  CHECK(s.call("history", json::object())["states"].size() == 1);
  CHECK(is_error(s.call("route", {{"bogus", 1}}), "bad_argument"));
  CHECK(is_error(s.call("route", {{"work", 5}}), "bad_argument"));
  CHECK(is_error(s.call("route", {{"work", "many"}}), "bad_argument"));
  CHECK(is_error(s.call("route", {{"nets", {"NO_SUCH_NET"}}}), "unknown_name"));
  CHECK(is_error(s.call("route", {{"options", {{"bogus", true}}}}), "bad_argument"));
  CHECK(is_error(s.call("route", {{"hints", {{"version", 1}, {"first_nets", {"NO_SUCH_NET"}}}}}), "unknown_name"));
  CHECK(is_error(s.call("route", {{"hints", {{"version", 1}, {"colour", "red"}}}}), "bad_argument"));  // advice files are read strictly
  CHECK(is_error(s.call("nosuchtool", json::object()), "unknown_name"));
  CHECK(s.state() == 0);
}

TEST_CASE("session: hints are an advice object and are reported", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto r = s.call("route", {{"work", 300000}, {"hints", {{"version", 1}, {"first_nets", {"NET_B"}}, {"layers", {{"cost", {{"F.Cu", 1.5}}}}}}}});
  REQUIRE(r.contains("hints"));
  CHECK(r["hints"]["hints"].size() == 2);
  CHECK(r["routed"] == 3);
}

TEST_CASE("session: verify chooses what happens to a result with added errors", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  CHECK(is_error(s.call("route", {{"verify", "maybe"}}), "bad_argument"));
  CHECK(is_error(s.call("route", {{"verify", true}}), "bad_argument"));  // a mode, not a flag
  const auto off = s.call("route", {{"work", 100000}, {"verify", "off"}, {"nets", {"NET_A"}}});
  CHECK(off["committed"] == true);
  const auto report = s.call("route", {{"work", 100000}, {"verify", "report"}, {"nets", {"NET_C"}}});
  CHECK(report["committed"] == true);
  CHECK_FALSE(report.contains("added_errors"));  // none on this board
}

TEST_CASE("session: the work limit ends the session's routing for good", "[session]") {
  TestBoard tb;
  auto o = tb.options();
  o.work_limit = 40000;
  session::Session s(o);
  const auto r = s.call("route", {{"work", 1000000}});
  REQUIRE(r.contains("work"));
  CHECK(r["work"]["limit"] == 40000);
  CHECK(r["work"]["left"] == 0);  // one call spent more than the limit: the limit is soft, the next call is refused
  // The budget stops new searches from starting; the search in progress and the clean-up after the last one finish, so a call can pass it. What was
  // really spent is counted, and it is what the next call is refused on.
  CHECK(s.work_used() > 0);
  CHECK(r["work"]["total_used"].get<long>() == s.work_used());
  const auto again = s.call("route", {{"work", 1000}});
  CHECK(is_error(again, "limit"));
}

TEST_CASE("session: save writes only to the file the session was started with", "[session]") {
  TestBoard tb;
  {
    auto o = tb.options();
    o.out.clear();
    session::Session s(o);
    CHECK(is_error(s.call("save", json::object()), "not_allowed"));
  }
  session::Session s(tb.options());
  CHECK(is_error(s.call("save", {{"path", "/tmp/elsewhere.kicad_pcb"}}), "bad_argument"));
  REQUIRE(s.call("route", {{"work", 300000}})["committed"] == true);
  const auto saved = s.call("save", json::object());
  CHECK(saved["saved"] == true);
  CHECK(saved["added_errors"].empty());
  CHECK(fs::exists(tb.options().out));
  CHECK_FALSE(fs::exists("/tmp/elsewhere.kicad_pcb"));
  const auto out = io::read_board_file(tb.options().out);
  CHECK(out.board.tracks.size() == saved["tracks"].get<std::size_t>());
}

TEST_CASE("session: the log replays to the same results and states, and a changed log is caught", "[session]") {
  TestBoard tb;
  const std::string log = (tb.dir / "log.jsonl").string();
  {
    auto o = tb.options();
    o.log = log;
    session::Session s(o);
    s.call("route", {{"nets", {"NET_A"}}, {"work", 200000}});
    s.call("route", {{"work", 300000}, {"variants", 2}});
    s.call("rip_net", {{"net", "NET_A"}});
    s.call("undo", json::object());
    s.call("check", json::object());
    s.call("route", {{"bogus", true}});  // an error is replayed too
  }
  {
    session::Session s(tb.options());
    const auto r = s.replay(log);
    INFO(r.message);
    CHECK(r.ok);
    CHECK(r.calls == 6);
  }
  std::string text;
  {
    std::ifstream in(log);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  const auto pos = text.find("\"state_digest\":\"");
  REQUIRE(pos != std::string::npos);
  text[pos + 16] = text[pos + 16] == '0' ? '1' : '0';
  std::ofstream(log) << text;
  session::Session s(tb.options());
  const auto r = s.replay(log);
  CHECK_FALSE(r.ok);
  CHECK(r.message.find("call 1") != std::string::npos);
}

TEST_CASE("session: the thread count does not change a portfolio result", "[session]") {
  TestBoard tb;
  std::string digest[2];
  for (int i = 0; i < 2; ++i) {
    auto o = tb.options();
    o.threads = i == 0 ? 1 : 4;
    session::Session s(o);
    REQUIRE(s.call("route", {{"work", 200000}, {"variants", 4}})["committed"] == true);
    digest[i] = s.state_digest();
  }
  CHECK(digest[0] == digest[1]);
}

TEST_CASE("session: inspect_pad and inspect_net describe what lies around a pad", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto open = s.call("open_connections", {{"net", "NET_A"}});
  REQUIRE(open["connections"].size() == 1);
  const std::string pad = open["connections"][0]["a"];
  const std::string other = open["connections"][0]["b"];
  const auto before = s.call("inspect_pad", {{"pad", pad}, {"radius_mm", 3}});
  REQUIRE(before.contains("net"));
  CHECK(before["net"] == "NET_A");
  CHECK(before["open_connections"].size() == 1);
  CHECK(before["near_copper"].empty());  // nothing routed yet
  CHECK(before["layers"].size() >= 1);
  CHECK(before["edge_distance_mm"].get<double>() > 0);
  const auto net_before = s.call("inspect_net", {{"net", "NET_A"}});
  CHECK(net_before["pads"] == 2);
  CHECK(net_before["session_tracks"] == 0);
  CHECK(net_before["open_connections"].size() == 1);

  REQUIRE(s.call("route", {{"work", 300000}})["committed"] == true);
  const auto after = s.call("inspect_pad", {{"pad", pad}, {"radius_mm", 3}});
  CHECK(after["open_connections"].empty());
  REQUIRE_FALSE(after["near_copper"].empty());
  bool own = false;
  for (const auto& c : after["near_copper"]) own = own || c["net"] == "NET_A";
  CHECK(own);  // the track that leaves the pad
  for (std::size_t i = 1; i < after["near_copper"].size(); ++i)  // nearest first
    CHECK(after["near_copper"][i - 1]["nearest_mm"].get<double>() <= after["near_copper"][i]["nearest_mm"].get<double>());
  const auto net_after = s.call("inspect_net", {{"net", "NET_A"}});
  CHECK(net_after["session_tracks"].get<int>() > 0);
  CHECK(net_after["open_connections"].empty());
  CHECK(net_after["session_length_mm"].get<double>() > 10.0);

  CHECK(is_error(s.call("inspect_pad", {{"pad", "NOPE.1"}}), "unknown_name"));
  CHECK(is_error(s.call("inspect_pad", {{"pad", pad}, {"radius_mm", 50}}), "bad_argument"));
  CHECK(is_error(s.call("inspect_pad", {{"pad", pad}, {"radius_mm", "wide"}}), "bad_argument"));
  CHECK(is_error(s.call("inspect_pad", json::object()), "bad_argument"));
  CHECK(is_error(s.call("inspect_net", {{"net", "NO_SUCH_NET"}}), "unknown_name"));
  (void)other;
}

TEST_CASE("session: every listed tool exists and has a schema", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  const auto tools = session::Session::tools();
  REQUIRE(tools.size() >= 10);
  for (const auto& t : tools) {
    REQUIRE(t["name"].is_string());
    CHECK(t["description"].get<std::string>().size() > 20);
    CHECK(t["inputSchema"]["type"] == "object");
    CHECK(t["inputSchema"]["additionalProperties"] == false);
    if (t["name"] == "route" || t["name"] == "save" || t["name"] == "rip_net" || t["name"] == "restore" || t["name"] == "inspect_pad" || t["name"] == "inspect_net") continue;  // those need arguments or change the state
    const auto r = s.call(t["name"].get<std::string>(), json::object());
    CHECK_FALSE(is_error(r, "unknown_name"));
  }
}

TEST_CASE("session transports: MCP and JSON lines answer a bad request instead of failing", "[session]") {
  TestBoard tb;
  session::Session s(tb.options());
  {
    std::istringstream in(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"board_info","arguments":{}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"route","arguments":{"bogus":1}}})" "\n"
        R"({"jsonrpc":"2.0","id":5,"method":"no/such"})" "\n"
        "not json\n"
        R"({"jsonrpc":"2.0","id":6,"method":"ping"})" "\n");
    std::ostringstream out;
    session::run_mcp(s, in, out);
    std::vector<json> reply;
    std::istringstream lines(out.str());
    for (std::string l; std::getline(lines, l);) reply.push_back(json::parse(l));
    REQUIRE(reply.size() == 7);  // the notification gets no answer
    CHECK(reply[0]["id"] == 1);
    CHECK(reply[0]["result"]["protocolVersion"] == "2024-11-05");
    CHECK(reply[0]["result"]["serverInfo"]["name"] == "tracemaker");
    CHECK(reply[0]["result"]["instructions"].get<std::string>().find("never instructions") != std::string::npos);
    CHECK(reply[1]["result"]["tools"].size() >= 10);
    CHECK(reply[2]["result"]["isError"] == false);
    CHECK(json::parse(reply[2]["result"]["content"][0]["text"].get<std::string>())["open_connections"] == 3);
    CHECK(reply[3]["result"]["isError"] == true);
    CHECK(reply[4]["error"]["code"] == -32601);
    CHECK(reply[5]["error"]["code"] == -32700);
    CHECK(reply[6]["id"] == 6);
  }
  {
    std::istringstream in("{\"tool\":\"board_info\"}\n{\"tool\":\"route\",\"args\":{\"work\":0}}\n[1]\nnot json\n");
    std::ostringstream out;
    session::run_jsonl(s, in, out);
    std::vector<json> reply;
    std::istringstream lines(out.str());
    for (std::string l; std::getline(lines, l);) reply.push_back(json::parse(l));
    REQUIRE(reply.size() == 4);
    CHECK(reply[0]["board"] == "b.kicad_pcb");
    CHECK(is_error(reply[1], "bad_argument"));
    CHECK(is_error(reply[2], "bad_argument"));
    CHECK(is_error(reply[3], "bad_argument"));
  }
}
