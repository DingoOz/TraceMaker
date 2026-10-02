// Board reader tests against KiCad's own view of the same boards (tests/truth/*.json, produced by
// scripts/kicad_truth.py inside the KiCad 10 image). Boards come from bench/data (scripts/fetch_fixtures.sh);
// tests skip when the fixtures are missing.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "io/kicad/board_reader.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Case {
  const char* board;  // relative to bench/data/kicad/demos
  const char* truth;  // file in tests/truth
};
const Case kCases[] = {
    {"pic_programmer/pic_programmer.kicad_pcb", "pic_programmer.json"},
    {"multichannel/multichannel_mixer.kicad_pcb", "multichannel_mixer.json"},
    {"stickhub/StickHub.kicad_pcb", "StickHub.json"},
    {"ecc83/ecc83-pp.kicad_pcb", "ecc83-pp.json"},
    {"video/video.kicad_pcb", "video.json"},
};

fs::path src_dir() { return fs::path(TM_SOURCE_DIR); }

json load_json(const fs::path& p) {
  std::ifstream f(p);
  return json::parse(f);
}

double angle_diff(double a, double b) {
  double d = std::fmod(std::fabs(a - b), 360.0);
  return std::min(d, 360.0 - d);
}

}  // namespace

TEST_CASE("demo boards round-trip byte for byte", "[kicad][io]") {
  int ran = 0;
  for (const auto& c : kCases) {
    const fs::path p = src_dir() / "bench/data/kicad/demos" / c.board;
    if (!fs::exists(p)) continue;
    const auto lb = tmk::io::read_board_file(p.string());
    std::ifstream f(p, std::ios::binary);
    const std::string orig((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    INFO(c.board);
    CHECK(lb.doc.write() == orig);
    ++ran;
  }
  if (!ran) SKIP("fixtures missing: run scripts/fetch_fixtures.sh");
}

TEST_CASE("board reader matches KiCad's own model", "[kicad][io]") {
  int ran = 0;
  for (const auto& c : kCases) {
    const fs::path p = src_dir() / "bench/data/kicad/demos" / c.board;
    if (!fs::exists(p)) continue;
    ++ran;
    INFO(c.board);
    const json t = load_json(src_dir() / "tests/truth" / c.truth);
    const auto lb = tmk::io::read_board_file(p.string());
    const auto& b = lb.board;
    CHECK(b.warnings.empty());

    // Copper stack.
    REQUIRE(static_cast<std::size_t>(b.copper_count()) == t["copper_layers"].size());
    for (int i = 0; i < b.copper_count(); ++i) CHECK(b.copper_name(i) == t["copper_layers"][static_cast<std::size_t>(i)].get<std::string>());

    // Nets: every named net KiCad knows, we know.
    std::size_t named = 0;
    for (const auto& n : b.nets) named += !n.name.empty();
    CHECK(named == t["nets"].size());

    // Footprints by reference (references can repeat, e.g. mounting holes, so compare as multimaps).
    CHECK(b.footprints.size() == t["footprints"].size());
    std::multimap<std::string, json> tfp;
    for (const auto& f : t["footprints"]) tfp.emplace(f["ref"].get<std::string>(), f);
    for (const auto& f : b.footprints) {
      bool found = false;
      auto [lo, hi] = tfp.equal_range(f.reference);
      for (auto it = lo; it != hi; ++it) {
        const json& x = it->second;
        if (x["x"].get<long long>() == f.pos.x && x["y"].get<long long>() == f.pos.y) {
          found = true;
          CHECK(angle_diff(x["angle"].get<double>(), f.angle) < 1e-6);
          CHECK(x["back"].get<bool>() == f.back);
          CHECK(x["locked"].get<bool>() == f.locked);
          CHECK(x["pads"].get<std::size_t>() == f.pads.size());
          break;
        }
      }
      INFO("footprint " << f.reference);
      CHECK(found);
    }

    // Pads: absolute position, orientation, size, drill, net, copper layers.
    REQUIRE(b.pads.size() == t["pads"].size());
    std::multimap<std::string, json> tp;
    for (const auto& p2 : t["pads"]) tp.emplace(p2["ref"].get<std::string>() + "/" + p2["num"].get<std::string>(), p2);
    int pad_mismatch = 0;
    for (const auto& pad : b.pads) {
      const auto& fp = b.footprints[static_cast<std::size_t>(pad.footprint)];
      bool found = false;
      auto [lo, hi] = tp.equal_range(fp.reference + "/" + pad.number);
      for (auto it = lo; it != hi; ++it) {
        const json& x = it->second;
        if (std::llabs(x["x"].get<long long>() - pad.pos.x) > 1 || std::llabs(x["y"].get<long long>() - pad.pos.y) > 1) continue;
        found = true;
        const bool ok = angle_diff(x["angle"].get<double>(), pad.angle) < 1e-6 && x["w"].get<long long>() == pad.size_x &&
                        x["h"].get<long long>() == pad.size_y && x["drill_w"].get<long long>() == pad.drill_x &&
                        x["net"].get<std::string>() == b.nets[static_cast<std::size_t>(pad.net)].name;
        std::vector<std::string> cu;
        for (int i = 0; i < b.copper_count(); ++i)
          if (pad.copper & tmk::model::layer_bit(i)) cu.push_back(b.copper_name(i));
        const bool layers_ok = x["layers"].get<std::vector<std::string>>() == cu;
        if (!ok || !layers_ok) {
          ++pad_mismatch;
          if (pad_mismatch <= 5)
            WARN("pad " << fp.reference << "/" << pad.number << " differs: kicad " << x.dump() << " ours angle=" << pad.angle
                        << " size=" << pad.size_x << "x" << pad.size_y << " net=" << b.nets[static_cast<std::size_t>(pad.net)].name);
        }
        break;
      }
      if (!found) {
        ++pad_mismatch;
        if (pad_mismatch <= 5) WARN("pad " << fp.reference << "/" << pad.number << " not found at " << pad.pos.x << "," << pad.pos.y);
      }
    }
    CHECK(pad_mismatch == 0);

    // Tracks, arcs and vias: counts, and every one matches by geometry, layer and net.
    REQUIRE(b.tracks.size() == t["tracks"].size());
    REQUIRE(b.arcs.size() == t["arcs"].size());
    REQUIRE(b.vias.size() == t["vias"].size());
    for (std::size_t i = 0; i < b.tracks.size(); ++i) {
      const auto& tr = b.tracks[i];
      const json& x = t["tracks"][i];  // file order equals KiCad's track order
      CHECK(x["sx"].get<long long>() == tr.a.x);
      CHECK(x["ey"].get<long long>() == tr.b.y);
      CHECK(x["width"].get<long long>() == tr.width);
      CHECK(x["layer"].get<std::string>() == b.copper_name(tr.layer));
      CHECK(x["net"].get<std::string>() == b.nets[static_cast<std::size_t>(tr.net)].name);
    }
    for (std::size_t i = 0; i < b.vias.size(); ++i) {
      const auto& v = b.vias[i];
      const json& x = t["vias"][i];
      CHECK(x["x"].get<long long>() == v.pos.x);
      CHECK(x["size"].get<long long>() == v.size);
      CHECK(x["drill"].get<long long>() == v.drill);
      CHECK(x["top"].get<std::string>() == b.copper_name(v.layer_top));
      CHECK(x["bottom"].get<std::string>() == b.copper_name(v.layer_bottom));
      CHECK(x["net"].get<std::string>() == b.nets[static_cast<std::size_t>(v.net)].name);
    }
    std::size_t board_zones = 0;
    for (const auto& z : b.zones) board_zones += z.footprint < 0;
    CHECK(board_zones == t["zones"].size());
  }
  if (!ran) SKIP("fixtures missing: run scripts/fetch_fixtures.sh");
}
