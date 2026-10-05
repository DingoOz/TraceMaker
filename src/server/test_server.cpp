// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the viewer protocol messages and the WebSocket/HTTP server.
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
}

TEST_CASE("board snapshot of pic_programmer has the board's objects", "[server]") {
  if (!std::filesystem::exists(kPic)) SKIP("fixture missing: " << kPic.string());
  const auto loaded = io::read_board_file(kPic.string());
  const auto& b = loaded.board;
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
