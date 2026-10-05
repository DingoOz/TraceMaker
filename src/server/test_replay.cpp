// Tests for the replay log (plain and zstd) and the router's heatmap overlays (doc 09 §2–3, decision D45).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/heat_grid.hpp"
#include "route/router.hpp"
#include "server/messages.hpp"
#include "server/replay_log.hpp"

using nlohmann::json;
using namespace tmk;

namespace {

std::filesystem::path scratch(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / ("tm_replay_test_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return dir / name;
}

std::vector<std::string> sample_lines(std::size_t n) {
  std::vector<std::string> v;
  for (std::size_t i = 0; i < n; ++i)
    v.push_back("{\"t\":" + std::to_string(static_cast<double>(i) * 0.001) + ",\"type\":\"track_add\",\"track\":{\"id\":" + std::to_string(i) + ",\"a\":[" +
                std::to_string(1000000 + i * 250000) + ",2000000],\"b\":[" + std::to_string(1500000 + i * 250000) + ",2500000],\"w\":250000,\"layer\":" +
                std::to_string(i % 4) + ",\"net\":" + std::to_string(i % 37) + "}}");
  return v;
}

std::vector<std::string> read_all(const std::string& path, bool* compressed = nullptr) {
  server::ReplayReader r(path);
  if (compressed) *compressed = r.compressed();
  std::vector<std::string> out;
  std::string line;
  while (r.next_line(line)) out.push_back(line);
  return out;
}

void write_all(const std::string& path, const std::vector<std::string>& lines) {
  server::ReplayWriter w(path);
  for (const auto& l : lines) w.write_line(l);
}

}  // namespace

TEST_CASE("replay log round-trips plain and zstd", "[replay]") {
  const auto lines = sample_lines(30000);  // ~4 MB: several zstd frames
  const std::string plain = scratch("a.jsonl").string(), packed = scratch("a.jsonl.zst").string();
  write_all(plain, lines);
  write_all(packed, lines);
  bool c1 = true, c2 = false;
  CHECK(read_all(plain, &c1) == lines);
  CHECK(read_all(packed, &c2) == lines);
  CHECK_FALSE(c1);
  CHECK(c2);
  // Compact: the compressed log is a small fraction of the JSON text.
  CHECK(std::filesystem::file_size(packed) * 8 < std::filesystem::file_size(plain));
  // Detection is by content, not by name.
  const std::string renamed = scratch("renamed.jsonl").string();
  std::filesystem::copy_file(packed, renamed, std::filesystem::copy_options::overwrite_existing);
  CHECK(read_all(renamed) == lines);
}

TEST_CASE("replay log keeps empty lines, long lines and end_frame boundaries", "[replay]") {
  std::vector<std::string> lines = {"{\"type\":\"board\"}", "", std::string(3u << 20, 'x'), "{\"type\":\"log\"}"};
  const std::string path = scratch("b.zst").string();
  {
    server::ReplayWriter w(path);
    CHECK(w.compressed());
    w.write_line(lines[0]);
    w.end_frame();  // keyframe in a frame of its own
    w.end_frame();  // an empty frame is not written
    for (std::size_t i = 1; i < lines.size(); ++i) w.write_line(lines[i]);
    w.close();
    w.close();  // idempotent
  }
  CHECK(read_all(path) == lines);
}

TEST_CASE("a truncated compressed log yields its complete lines, a corrupt one throws", "[replay]") {
  const auto lines = sample_lines(30000);
  const std::string path = scratch("c.zst").string();
  write_all(path, lines);
  std::string bytes;
  {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), {});
  }
  // A run killed mid-write: the tail of the last frame is missing.
  const std::string cut = scratch("cut.zst").string();
  {
    std::ofstream out(cut, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - bytes.size() / 5));
  }
  const auto got = read_all(cut);
  REQUIRE(got.size() > lines.size() / 2);
  REQUIRE(got.size() < lines.size());
  CHECK(std::equal(got.begin(), got.end(), lines.begin()));  // a prefix, every line whole
  // Damage in the middle of a frame is detected (frames carry checksums).
  std::string bad = bytes;
  for (std::size_t i = bad.size() / 3; i < bad.size() / 3 + 64; ++i) bad[i] = static_cast<char>(bad[i] ^ 0x5a);
  const std::string corrupt = scratch("bad.zst").string();
  {
    std::ofstream out(corrupt, std::ios::binary);
    out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
  }
  CHECK_THROWS(read_all(corrupt));
}

TEST_CASE("heat grid sizing, scaling and message", "[heatmap]") {
  route::HeatGrid g;
  CHECK_FALSE(g.ready());
  g.init(geom::Box{0, 0, 100'000'000, 50'000'000}, 50'000);  // 100 x 50 mm
  REQUIRE(g.ready());
  CHECK(g.width() <= route::HeatGrid::kMaxCells);
  CHECK(g.height() <= route::HeatGrid::kMaxCells);
  CHECK(g.cell() % 1000 == 0);
  CHECK(static_cast<Coord>(g.width()) * g.cell() > 100'000'000);
  g.add({0, 0}, 100);
  g.add({0, 0}, 300);
  g.add({99'000'000, 49'000'000}, 1);
  g.add({-5, 0}, 1000);            // outside: ignored
  g.add({200'000'000, 0}, 1000);   // outside: ignored
  CHECK(g.max() == 400);
  const auto b = g.bytes();
  CHECK(b[0] == 255);
  std::uint8_t lo = 255;
  for (auto x : b)
    if (x) lo = std::min(lo, x);
  CHECK(lo == 13);  // round(255 * sqrt(1 / 400)) = 12.75
  CHECK(std::count_if(b.begin(), b.end(), [](std::uint8_t v) { return v > 0; }) == 2);
  const auto j = json::parse(g.message("expansions", -1));
  CHECK(j["type"] == "heatmap");
  CHECK(server::message_type(g.message("expansions", -1)) == "heatmap");
  CHECK(j["name"] == "expansions");
  CHECK(j["w"] == g.width());
  CHECK(j["h"] == g.height());
  CHECK(j["layer"] == -1);
  CHECK(j["max"] == 400);
  CHECK(j["scale"] == "sqrt");
  CHECK(j["data"].size() == static_cast<std::size_t>(g.width() * g.height()));
  // raise() keeps the maximum, so filling it from an unordered container gives one answer.
  route::HeatGrid p, q;
  p.init(geom::Box{0, 0, 1'000'000, 1'000'000}, 10'000);
  q.init(geom::Box{0, 0, 1'000'000, 1'000'000}, 10'000);
  const std::vector<std::pair<geom::Point, std::uint64_t>> v = {{{5, 5}, 3}, {{6, 6}, 9}, {{500'000, 5}, 2}, {{7, 7}, 4}};
  for (const auto& [pt, x] : v) p.raise(pt, x);
  for (auto it = v.rbegin(); it != v.rend(); ++it) q.raise(it->first, it->second);
  CHECK(p.bytes() == q.bytes());
  CHECK(p.max() == 9);
  p.clear();
  CHECK(p.max() == 0);
}

namespace {

// Collects messages like the viewer server: transient ones wanted, never blocks.
struct CaptureSink final : events::Sink {
  std::mutex m;
  std::vector<std::string> msgs;
  bool transient = true;
  void publish(std::string json) override {
    std::lock_guard<std::mutex> lk(m);
    msgs.push_back(std::move(json));
  }
  bool wants_transient() const override { return transient; }
};

std::string copper_text(const route::RouteResult& r) {
  std::string s;
  for (const auto& t : r.tracks)
    s += std::to_string(t.a.x) + "," + std::to_string(t.a.y) + "," + std::to_string(t.b.x) + "," + std::to_string(t.b.y) + "," +
         std::to_string(t.width) + "," + std::to_string(t.layer) + "," + std::to_string(t.net) + "\n";
  for (const auto& v : r.vias)
    s += std::to_string(v.pos.x) + "," + std::to_string(v.pos.y) + "," + std::to_string(v.size) + "," + std::to_string(v.drill) + "," +
         std::to_string(v.layer_top) + "," + std::to_string(v.layer_bottom) + "," + std::to_string(v.net) + "\n";
  return s;
}

}  // namespace

TEST_CASE("router heatmaps: emitted with a sink, routing unchanged by it", "[heatmap][route]") {
  const auto path = std::filesystem::path(TM_SOURCE_DIR) / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/C-BISCUIT_buck-reg-5v/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " << path.string());
  const auto lb = io::read_board_file(path.string());
  const auto rules = io::read_design_rules(path.string());
  route::RouterOptions o;
  o.work_budget = 2'000'000;
  o.gpu_device = -1;
  o.time_limit_s = 600;
  const auto plain = route::Router(lb.board, rules, o).run();
  for (const bool transient : {true, false}) {  // viewer (frontiers wanted) and recorder
    CaptureSink sink;
    sink.transient = transient;
    route::RouterOptions ov = o;
    ov.sink = &sink;
    const auto viewed = route::Router(lb.board, rules, ov).run();
    CHECK(copper_text(viewed) == copper_text(plain));
    CHECK(viewed.routed == plain.routed);
    CHECK(viewed.expansions == plain.expansions);
    int exp = 0, hist = 0;
    for (const auto& m : sink.msgs) {
      if (server::message_type(m) != "heatmap") continue;
      const auto j = json::parse(m);
      REQUIRE(j["data"].size() == static_cast<std::size_t>(j["w"].get<int>() * j["h"].get<int>()));
      if (j["name"] == "expansions") {
        ++exp;
        CHECK(j["max"].get<long>() > 0);
      } else if (j["name"] == "history") {
        ++hist;
      }
    }
    CHECK(exp >= 1);
    CHECK(hist == exp);  // sent together
    // The last message before the final stats is the complete picture.
    CHECK(server::message_type(sink.msgs.back()) == "stage");
  }
}
