// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the viewer protocol messages and the WebSocket/HTTP server.
#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "io/kicad/board_reader.hpp"
#include "server/messages.hpp"
#include "server/viewer_server.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using nlohmann::json;
using namespace tmk;

namespace {

const std::filesystem::path kPic = std::filesystem::path(TM_SOURCE_DIR) / "bench/data/kicad/demos/pic_programmer/pic_programmer.kicad_pcb";

struct WsClient {
  net::io_context ioc;
  websocket::stream<beast::tcp_stream> ws{ioc};
  explicit WsClient(std::uint16_t port) {
    tcp::resolver r(ioc);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(ws).connect(r.resolve("127.0.0.1", std::to_string(port)));
    ws.handshake("127.0.0.1", "/ws");
  }
  json read() {
    beast::flat_buffer buf;
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
    ws.read(buf);
    return json::parse(beast::buffers_to_string(buf.data()));
  }
};

http::response<http::string_body> http_get(std::uint16_t port, const std::string& target) {
  net::io_context ioc;
  beast::tcp_stream s(ioc);
  tcp::resolver r(ioc);
  s.expires_after(std::chrono::seconds(10));
  s.connect(r.resolve("127.0.0.1", std::to_string(port)));
  http::request<http::empty_body> req{http::verb::get, target, 11};
  req.set(http::field::host, "127.0.0.1");
  req.keep_alive(false);
  http::write(s, req);
  beast::flat_buffer buf;
  http::response<http::string_body> res;
  http::read(s, buf, res);
  return res;
}

}  // namespace

TEST_CASE("message builders put type first and classify", "[server]") {
  const auto m = server::track_add(7, {1, 2}, {3, 4}, 250'000, 1, 5);
  CHECK(m.starts_with("{\"type\":\"track_add\""));
  CHECK(server::message_type(m) == "track_add");
  CHECK(server::message_type(R"({"text":"x","type":"log"})") == "log");
  CHECK(server::message_type("not json").empty());
  const auto j = json::parse(m);
  CHECK(j["track"]["id"] == 7);
  CHECK(j["track"]["a"] == json::array({1, 2}));
  CHECK(j["track"]["w"] == 250'000);

  server::Stats st;
  st.stage = "route";
  st.routed = 3;
  st.total = 10;
  st.extra = {{"gpu", 0.5}};
  const auto sj = json::parse(server::stats(st));
  CHECK(sj["routed"] == 3);
  CHECK(sj["extra"]["gpu"] == 0.5);

  server::Failure f;
  f.net = 4;
  f.cause = "blocked";
  f.blockers = {1, 2};
  const auto fj = json::parse(server::failure(f));
  CHECK(fj["type"] == "failure");
  CHECK(fj["blockers"].size() == 2);
  CHECK_FALSE(fj.contains("region"));
  // The diagnostic fields are optional: a plain failure has none of them, so an old reader sees what it always saw.
  for (const char* k : {"blocker_nets", "reason", "width_nm", "clearance_nm", "min_width_nm", "gap_nm", "gap_fixed_nm", "gap_layer", "at", "verdict", "rule", "rejected"})
    CHECK_FALSE(fj.contains(k));

  server::Failure d = f;
  d.blocker_nets = {7};
  d.reason = "boxed_in";
  d.width_nm = 250'000;
  d.clearance_nm = 200'000;
  d.min_width_nm = 100'000;
  d.gap_nm = 0;
  d.gap_fixed_nm = 120'000;
  d.gap_layer = 1;
  d.at = model::Point{-600'000, 0};
  d.verdict = "placement";
  d.rule = "netclass Default: width 0.25 mm, clearance 0.2 mm";
  d.rejected = {{{-7'950'000, 0}, 0}, {{-600'000, 0}, 0}};
  const auto dj = json::parse(server::failure(d));
  CHECK(dj["type"] == "failure");
  CHECK(dj["width_nm"] == 250'000);
  CHECK(dj["clearance_nm"] == 200'000);
  CHECK(dj["min_width_nm"] == 100'000);
  CHECK(dj["gap_nm"] == 0);  // a gap of zero is a value, not "unset"
  CHECK(dj["gap_fixed_nm"] == 120'000);
  CHECK(dj["gap_layer"] == 1);
  CHECK(dj["at"] == json::array({-600'000, 0}));
  CHECK(dj["verdict"] == "placement");
  CHECK(dj["blocker_nets"] == json::array({7}));
  REQUIRE(dj["rejected"].size() == 2);
  CHECK(dj["rejected"][1] == json::array({-600'000, 0, 0}));
  CHECK(dj["blockers"].size() == 2);  // the old fields are untouched
}

TEST_CASE("board snapshot of pic_programmer has the board's objects", "[server]") {
  if (!std::filesystem::exists(kPic)) SKIP("fixture missing: " << kPic.string());
  const auto loaded = io::read_board_file(kPic.string());
  const auto& b = loaded.board;
  CHECK_FALSE(json::parse(server::board_snapshot_json(b, "pic_programmer")).contains("freeze"));  // on by default: nothing said
  CHECK(json::parse(server::board_snapshot_json(b, "pic_programmer", false))["freeze"] == false);
  const auto j = json::parse(server::board_snapshot_json(b, "pic_programmer"));
  CHECK(j["type"] == "board");
  CHECK(j["name"] == "pic_programmer");
  REQUIRE(j["bbox"].size() == 4);
  CHECK(j["bbox"][2].get<std::int64_t>() > j["bbox"][0].get<std::int64_t>());
  CHECK(j["layers"].size() == static_cast<std::size_t>(b.copper_count()));
  CHECK(j["layers"][0]["name"] == "F.Cu");
  CHECK(j["layers"].back()["name"] == "B.Cu");
  CHECK(j["tracks"].size() >= b.tracks.size());
  CHECK(j["vias"].size() == b.vias.size());
  CHECK(j["footprints"].size() == b.footprints.size());
  CHECK(j["nets"].size() >= b.nets.size());
  // Every pad with copper appears (some pads have several shapes).
  std::set<int> pad_ids;
  for (const auto& p : j["pads"]) pad_ids.insert(p["id"].get<int>());
  std::size_t with_copper = 0;
  for (const auto& p : b.pads) with_copper += p.copper != 0;
  CHECK(pad_ids.size() >= with_copper);
  CHECK(with_copper > 100);
  // Track ids are their board index; nets index the nets list.
  for (std::size_t i = 0; i < b.tracks.size(); ++i) {
    CHECK(j["tracks"][i]["id"] == static_cast<std::int64_t>(i));
    CHECK(j["tracks"][i]["net"] == b.tracks[i].net);
  }
  // The outline is one closed loop.
  REQUIRE(j["outline"].size() >= 1);
  const auto& loop = j["outline"][0];
  REQUIRE(loop.size() >= 4);
  CHECK(std::abs(loop.front()[0].get<std::int64_t>() - loop.back()[0].get<std::int64_t>()) <= 2000);
  CHECK(std::abs(loop.front()[1].get<std::int64_t>() - loop.back()[1].get<std::int64_t>()) <= 2000);
  // Footprint boxes contain the footprint's pads.
  for (const auto& fp : j["footprints"]) CHECK(fp["bbox"][0].get<std::int64_t>() <= fp["bbox"][2].get<std::int64_t>());
  // Zones carry fill polygons.
  for (const auto& z : j["zones"]) CHECK(!z["polys"].empty());
}

TEST_CASE("viewer server serves files, streams snapshot then deltas, shuts down", "[server]") {
  const auto dir = std::filesystem::temp_directory_path() / ("tm_server_test_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir / "assets");
  std::ofstream(dir / "index.html") << "<!doctype html><title>viewer</title>";
  std::ofstream(dir / "assets" / "app.js") << "console.log(1)";
  std::ofstream(dir.parent_path() / "tm_server_secret.txt") << "secret";

  server::ServerOptions o;
  o.host = "127.0.0.1";
  o.port = 0;
  o.web_root = dir.string();
  {
    server::ViewerServer srv(o);
    REQUIRE(srv.port() != 0);
    CHECK_FALSE(srv.wants_transient());

    auto idx = http_get(srv.port(), "/");
    CHECK(idx.result() == http::status::ok);
    CHECK(idx.body().find("viewer") != std::string::npos);
    CHECK(idx[http::field::content_type].starts_with("text/html"));
    auto js = http_get(srv.port(), "/assets/app.js?v=1");
    CHECK(js.result() == http::status::ok);
    CHECK(js[http::field::content_type].starts_with("text/javascript"));
    CHECK(http_get(srv.port(), "/missing.css").result() == http::status::not_found);
    CHECK(http_get(srv.port(), "/../tm_server_secret.txt").result() != http::status::ok);
    CHECK(http_get(srv.port(), "/%2e%2e/tm_server_secret.txt").result() != http::status::ok);
    CHECK(http_get(srv.port(), "/assets/%2E%2E/%2E%2E/tm_server_secret.txt").result() != http::status::ok);

    model::Board b;
    srv.publish(server::board_snapshot_json(b, "empty"));
    srv.publish(server::track_add(0, {0, 0}, {1'000'000, 0}, 200'000, 0, 0));

    WsClient c1(srv.port());
    auto m1 = c1.read();
    CHECK(m1["type"] == "board");
    CHECK(m1["name"] == "empty");
    auto m2 = c1.read();  // state since the snapshot is replayed to a late client
    CHECK(m2["type"] == "track_add");

    srv.publish(server::log("info", "hello"));
    auto m3 = c1.read();
    CHECK(m3["type"] == "log");
    CHECK(m3["text"] == "hello");
    CHECK(srv.client_count() == 1);
    CHECK(srv.wants_transient());

    // Removing a track added after the snapshot cancels it for new clients.
    srv.publish(server::track_remove(0));
    srv.publish(server::via_add(3, model::Via{}));
    CHECK(c1.read()["type"] == "track_remove");
    CHECK(c1.read()["type"] == "via_add");
    WsClient c2(srv.port());
    CHECK(c2.read()["type"] == "board");
    CHECK(c2.read()["type"] == "via_add");
    CHECK(c2.read()["type"] == "log");
  }  // destructor shuts the server down with clients connected
  std::filesystem::remove_all(dir);
  std::filesystem::remove(dir.parent_path() / "tm_server_secret.txt");
}

TEST_CASE("slow clients lose transient messages but not state", "[server]") {
  server::ServerOptions o;
  o.host = "127.0.0.1";
  o.port = 0;
  o.max_queue_msgs = 50;
  server::ViewerServer srv(o);
  WsClient c(srv.port());
  srv.publish(server::board_snapshot_json(model::Board{}, "b"));
  CHECK(c.read()["type"] == "board");
  // Flood without reading: frontier messages beyond the bound are dropped, track_adds survive.
  std::vector<model::Point> pts(200, model::Point{1, 2});
  for (int i = 0; i < 3000; ++i) {
    srv.publish(server::frontier(i, 0, pts));
    if (i % 100 == 0) srv.publish(server::track_add(i, {0, 0}, {1, 1}, 1, 0, 0));
    if (i % 10 == 0) srv.publish(server::stats({}));
  }
  srv.publish(server::log("info", "end"));
  int adds = 0, frontiers = 0;
  for (;;) {
    const auto m = c.read();
    if (m["type"] == "track_add") ++adds;
    if (m["type"] == "frontier") ++frontiers;
    if (m["type"] == "log") break;
  }
  CHECK(adds == 30);
  CHECK(frontiers < 3000);
  CHECK(srv.counters().dropped > 0);
}

// ---------------------------------------------------------------------------------------------------------
// Who may connect (issue #4, doc 09 "Security", D116).
namespace {

struct Reply {
  http::status status = http::status::internal_server_error;
  std::string body, set_cookie, referrer_policy;
};

Reply get_with(std::uint16_t port, const std::string& target, const std::string& host, const std::string& cookie = "") {
  net::io_context ioc;
  beast::tcp_stream s(ioc);
  tcp::resolver r(ioc);
  s.expires_after(std::chrono::seconds(10));
  s.connect(r.resolve("127.0.0.1", std::to_string(port)));
  http::request<http::empty_body> req{http::verb::get, target, 11};
  req.set(http::field::host, host);
  if (!cookie.empty()) req.set(http::field::cookie, cookie);
  req.keep_alive(false);
  http::write(s, req);
  beast::flat_buffer buf;
  http::response<http::string_body> res;
  http::read(s, buf, res);
  return {res.result(), res.body(), std::string(res[http::field::set_cookie]), std::string(res["Referrer-Policy"])};
}

// A WebSocket handshake with the given headers: the status of the answer (101 = accepted).
http::status ws_status(std::uint16_t port, const std::string& target, const std::string& host, const std::string& origin = "", const std::string& cookie = "") {
  net::io_context ioc;
  websocket::stream<beast::tcp_stream> ws{ioc};
  tcp::resolver r(ioc);
  beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
  beast::get_lowest_layer(ws).connect(r.resolve("127.0.0.1", std::to_string(port)));
  ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
    if (!origin.empty()) req.set(http::field::origin, origin);
    if (!cookie.empty()) req.set(http::field::cookie, cookie);
  }));
  websocket::response_type res;
  beast::error_code ec;
  ws.handshake(res, host, target, ec);
  return res.result();
}

server::ServerOptions test_options(const std::filesystem::path& dir, const std::string& host) {
  server::ServerOptions o;
  o.host = host;
  o.port = 0;
  o.web_root = dir.string();
  return o;
}

struct WebDir {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("tm_server_auth_" + std::to_string(::getpid()));
  WebDir() {
    std::filesystem::create_directories(dir / "assets");
    std::ofstream(dir / "index.html") << "<!doctype html><title>viewer</title>";
    std::ofstream(dir / "assets" / "app.js") << "console.log(1)";
  }
  ~WebDir() { std::filesystem::remove_all(dir); }
};

}  // namespace

TEST_CASE("viewer server: by default only this machine can reach it, and a page on another site cannot read the stream", "[server][security]") {
  WebDir w;
  CHECK(server::ServerOptions{}.host == "127.0.0.1");  // the default, not 0.0.0.0
  server::ViewerServer srv(test_options(w.dir, "127.0.0.1"));
  const auto port = srv.port();
  CHECK_FALSE(srv.exposed());
  CHECK(srv.token().empty());
  CHECK(srv.notice().empty());
  CHECK(srv.url() == "http://127.0.0.1:" + std::to_string(port) + "/");
  const std::string here = "127.0.0.1:" + std::to_string(port);
  CHECK(get_with(port, "/", here).status == http::status::ok);
  CHECK(get_with(port, "/", "localhost:" + std::to_string(port)).status == http::status::ok);
  CHECK(get_with(port, "/", "[::1]:" + std::to_string(port)).status == http::status::ok);
  // DNS rebinding: a name that is not this machine's, even if it resolves here, is refused.
  CHECK(get_with(port, "/", "evil.example:" + std::to_string(port)).status == http::status::forbidden);
  CHECK(get_with(port, "/assets/app.js", "evil.example").status == http::status::forbidden);
  // WebSocket: no Origin (a script or tool), the server's own origin, and the same page on localhost are fine ...
  CHECK(ws_status(port, "/ws", here) == http::status::switching_protocols);
  CHECK(ws_status(port, "/ws", here, "http://" + here) == http::status::switching_protocols);
  CHECK(ws_status(port, "/ws", "localhost:" + std::to_string(port), "http://localhost:" + std::to_string(port)) == http::status::switching_protocols);
  // ... a page of any other site is not: browsers do not apply the same-origin rule to WebSockets, the server has to.
  CHECK(ws_status(port, "/ws", here, "http://evil.example") == http::status::forbidden);
  CHECK(ws_status(port, "/ws", here, "http://evil.example:" + std::to_string(port)) == http::status::forbidden);
  CHECK(ws_status(port, "/ws", here, "http://localhost:9999") == http::status::forbidden);  // another port is another origin
  CHECK(ws_status(port, "/ws", here, "null") == http::status::forbidden);
  // Every answer says not to leak the address, and nothing was cached.
  CHECK(get_with(port, "/", here).referrer_policy == "no-referrer");
}

TEST_CASE("viewer server: a viewer other machines can reach needs its token, in the URL once and then in a cookie", "[server][security]") {
  WebDir w;
  server::ViewerServer srv(test_options(w.dir, "0.0.0.0"));
  const auto port = srv.port();
  const std::string host = "127.0.0.1:" + std::to_string(port);
  REQUIRE(srv.exposed());
  const std::string token = srv.token();
  REQUIRE(token.size() == 32);  // 128 random bits
  CHECK(std::all_of(token.begin(), token.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
  CHECK(srv.url().find("/?token=" + token) != std::string::npos);
  CHECK(srv.notice().find("other machines can reach it") != std::string::npos);
  server::ViewerServer other(test_options(w.dir, "0.0.0.0"));
  CHECK(other.token() != token);  // a new token every run

  // No token, a wrong one, one of the wrong length: the page, the files and the stream are all closed.
  for (const std::string& target : {std::string("/"), std::string("/assets/app.js"), std::string("/?token=nope"), "/?token=" + token.substr(1), "/?token=" + token + "0", std::string("/?other=" + token)}) {
    INFO(target);
    const auto r = get_with(port, target, host);
    CHECK(r.status == http::status::unauthorized);
    CHECK(r.body.find("needs its token") != std::string::npos);
    CHECK(r.body.find("<title>viewer</title>") == std::string::npos);  // nothing of the page itself
  }
  CHECK(ws_status(port, "/ws", host) == http::status::unauthorized);
  CHECK(ws_status(port, "/ws?token=nope", host) == http::status::unauthorized);
  CHECK(get_with(port, "/", host, "tm_token_" + std::to_string(port) + "=nope").status == http::status::unauthorized);

  // The token in the URL opens the page and is remembered in a cookie the page sends back with every request, the stream included.
  const auto first = get_with(port, "/?token=" + token, host);
  CHECK(first.status == http::status::ok);
  CHECK(first.body.find("<title>viewer</title>") != std::string::npos);
  const std::string cname = "tm_token_" + std::to_string(port);  // cookies ignore ports: the name carries it
  CHECK(first.set_cookie == cname + "=" + token + "; Path=/; HttpOnly; SameSite=Strict");
  const std::string cookie = "theme=dark; " + cname + "=" + token;
  const auto asset = get_with(port, "/assets/app.js", host, cookie);
  CHECK(asset.status == http::status::ok);
  CHECK(asset.set_cookie.empty());
  CHECK(ws_status(port, "/ws", host, "http://" + host, cookie) == http::status::switching_protocols);
  CHECK(ws_status(port, "/ws?token=" + token, host) == http::status::switching_protocols);  // where cookies are refused
  // The token does not make a foreign page welcome: its Origin is still checked.
  CHECK(ws_status(port, "/ws?token=" + token, host, "http://evil.example") == http::status::forbidden);
  CHECK(ws_status(port, "/ws", host, "http://evil.example", cookie) == http::status::forbidden);
}

TEST_CASE("viewer server: a token of your own, a viewer without one on purpose, and tokens that are refused", "[server][security]") {
  WebDir w;
  auto o = test_options(w.dir, "0.0.0.0");
  o.token = "my-own.token_0123456789";
  {
    server::ViewerServer srv(o);
    CHECK(srv.token() == o.token);
    const std::string host = "127.0.0.1:" + std::to_string(srv.port());
    CHECK(get_with(srv.port(), "/?token=my-own.token_0123456789", host).status == http::status::ok);
    CHECK(get_with(srv.port(), "/", host).status == http::status::unauthorized);
  }
  o.token = "";
  o.allow_open = true;  // --view-insecure
  {
    server::ViewerServer srv(o);
    CHECK(srv.token().empty());
    CHECK(srv.exposed());
    CHECK(srv.notice().find("WITHOUT a token") != std::string::npos);
    const std::string host = "127.0.0.1:" + std::to_string(srv.port());
    CHECK(get_with(srv.port(), "/", host).status == http::status::ok);
    CHECK(ws_status(srv.port(), "/ws", host) == http::status::switching_protocols);
    CHECK(ws_status(srv.port(), "/ws", host, "http://evil.example") == http::status::forbidden);  // the origin check stays
  }
  for (const char* bad : {"short", "0123456789abcde", "has a space in it 1234", "slash/in/the/token/1234", "semi;colon;in;it;1234567"}) {
    auto b = test_options(w.dir, "0.0.0.0");
    b.token = bad;
    INFO(bad);
    CHECK_THROWS_AS(server::ViewerServer(b), std::runtime_error);
  }
  // A token on a loopback viewer is allowed too (a shared machine).
  auto l = test_options(w.dir, "127.0.0.1");
  l.token = "loopback-token-0123456789";
  server::ViewerServer srv(l);
  CHECK(srv.token() == l.token);
  CHECK(get_with(srv.port(), "/", "127.0.0.1:" + std::to_string(srv.port())).status == http::status::unauthorized);
}

TEST_CASE("viewer server: the number of viewers and connections is capped", "[server][security]") {
  WebDir w;
  auto o = test_options(w.dir, "127.0.0.1");
  o.max_clients = 2;
  o.max_connections = 4;
  server::ViewerServer srv(o);
  const std::string host = "127.0.0.1:" + std::to_string(srv.port());
  net::io_context ioc;
  websocket::stream<beast::tcp_stream> a{ioc}, b{ioc};
  tcp::resolver r(ioc);
  for (auto* ws : {&a, &b}) {
    beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(*ws).connect(r.resolve("127.0.0.1", std::to_string(srv.port())));
    ws->handshake(host, "/ws");
  }
  for (int i = 0; i < 100 && srv.client_count() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(srv.client_count() == 2);
  CHECK(ws_status(srv.port(), "/ws", host) == http::status::service_unavailable);  // the third viewer is told so
  CHECK(get_with(srv.port(), "/", host).status == http::status::ok);               // the page is still served
  // Connections that only sit there (a slow-loris) are closed once there are too many; the ones before them and the page are unaffected.
  std::vector<std::unique_ptr<tcp::socket>> idle;
  for (int i = 0; i < 4; ++i) {
    idle.push_back(std::make_unique<tcp::socket>(ioc));
    idle.back()->connect({net::ip::make_address("127.0.0.1"), srv.port()});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  tcp::socket extra(ioc);
  extra.connect({net::ip::make_address("127.0.0.1"), srv.port()});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  char c;
  beast::error_code ec;
  extra.non_blocking(false);
  const std::size_t n = extra.read_some(net::buffer(&c, 1), ec);
  CHECK((ec == net::error::eof || ec == net::error::connection_reset));  // closed at once, nothing served
  CHECK(n == 0);
}

TEST_CASE("viewer server: what the review of the first version found", "[server][security]") {
  WebDir w;
  // Host values that are not a name and a numeric port are refused by a loopback viewer, however they start.
  {
    server::ViewerServer srv(test_options(w.dir, "127.0.0.1"));
    const auto port = srv.port();
    const std::string p = std::to_string(port);
    for (const std::string& bad : std::vector<std::string>{"localhost:80@evil.com", "127.0.0.1:" + p + ", evil.com", "evil.com#.localhost", "localhost:", "localhost:abc", "[::1", "[::1]x", "local host", "localhost:1:2", ""}) {
      INFO("Host: " << bad);
      CHECK(get_with(port, "/", bad).status == http::status::forbidden);
    }
    for (const std::string& good : std::vector<std::string>{"localhost:" + p, "LOCALHOST", "127.0.0.1", "[::1]:" + p, "[::1]", "app.localhost:" + p}) {
      INFO("Host: " << good);
      CHECK(get_with(port, "/", good).status == http::status::ok);
    }
  }
  // A cookie is for its own viewer: the token of one does not open another that has the same token on another port (cookies ignore ports).
  {
    auto o = test_options(w.dir, "0.0.0.0");
    o.token = "shared-token-0123456789";
    server::ViewerServer a(o), b(o);
    const std::string cookie_a = "tm_token_" + std::to_string(a.port()) + "=" + o.token;
    CHECK(get_with(a.port(), "/", "127.0.0.1:" + std::to_string(a.port()), cookie_a).status == http::status::ok);
    CHECK(get_with(b.port(), "/", "127.0.0.1:" + std::to_string(b.port()), cookie_a).status == http::status::unauthorized);
  }
  // An IPv6 address in the printed URL has its brackets.
  {
    server::ServerOptions o = test_options(w.dir, "::1");
    try {
      server::ViewerServer srv(o);
      CHECK(srv.url() == "http://[::1]:" + std::to_string(srv.port()) + "/");
    } catch (const std::runtime_error&) {
      SKIP("no IPv6 loopback here");
    }
  }
  // The default host follows TM_VIEW_HOST.
  {
    ::setenv("TM_VIEW_HOST", "0.0.0.0", 1);
    CHECK(server::default_host() == "0.0.0.0");
    CHECK(server::ServerOptions{}.host == "0.0.0.0");
    ::unsetenv("TM_VIEW_HOST");
    CHECK(server::default_host() == "127.0.0.1");
    ::setenv("TM_VIEW_HOST", "", 1);
    CHECK(server::default_host() == "127.0.0.1");  // an empty value is no value
    ::unsetenv("TM_VIEW_HOST");
  }
}

TEST_CASE("viewer server: a burst of upgrades cannot overshoot the cap, and one address cannot use up the connections", "[server][security]") {
  WebDir w;
  {
    auto o = test_options(w.dir, "127.0.0.1");
    o.max_clients = 4;
    o.max_connections = 128;
    o.max_connections_per_address = 128;
    server::ViewerServer srv(o);
    const std::string host = "127.0.0.1:" + std::to_string(srv.port());
    // 40 upgrades at once: the cap counts a session from the moment it is accepted, so exactly 4 get through however they interleave.
    std::vector<std::thread> threads;
    std::atomic<int> ok{0}, busy{0};
    std::vector<std::unique_ptr<net::io_context>> keep_ioc;
    std::vector<std::unique_ptr<websocket::stream<beast::tcp_stream>>> keep_ws;
    std::mutex mu;
    for (int i = 0; i < 40; ++i)
      threads.emplace_back([&] {
        auto ioc = std::make_unique<net::io_context>();
        auto ws = std::make_unique<websocket::stream<beast::tcp_stream>>(*ioc);
        tcp::resolver r(*ioc);
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws).connect(r.resolve("127.0.0.1", std::to_string(srv.port())));
        websocket::response_type res;
        beast::error_code ec;
        ws->handshake(res, host, "/ws", ec);
        (res.result() == http::status::switching_protocols ? ok : busy)++;
        std::lock_guard<std::mutex> lk(mu);  // keep the accepted ones open until the count is taken
        keep_ioc.push_back(std::move(ioc));
        keep_ws.push_back(std::move(ws));
      });
    for (auto& t : threads) t.join();
    CHECK(ok == 4);
    CHECK(busy == 36);
  }
  {
    // Idle sockets from one address stop at its share; the page is still served to a connection of the same address within it.
    auto o = test_options(w.dir, "127.0.0.1");
    o.max_connections = 64;
    o.max_connections_per_address = 8;
    server::ViewerServer srv(o);
    net::io_context ioc;
    std::vector<std::unique_ptr<tcp::socket>> idle;
    for (int i = 0; i < 8; ++i) {
      idle.push_back(std::make_unique<tcp::socket>(ioc));
      idle.back()->connect({net::ip::make_address("127.0.0.1"), srv.port()});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tcp::socket extra(ioc);
    extra.connect({net::ip::make_address("127.0.0.1"), srv.port()});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    char c;
    beast::error_code ec;
    CHECK(extra.read_some(net::buffer(&c, 1), ec) == 0);  // closed at once
    CHECK((ec == net::error::eof || ec == net::error::connection_reset));
    idle.clear();  // the share is free again
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(get_with(srv.port(), "/", "127.0.0.1").status == http::status::ok);
  }
}
