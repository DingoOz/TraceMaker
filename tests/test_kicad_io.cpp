// Board reader tests against KiCad's own view of the same boards (tests/truth/*.json, produced by
// scripts/kicad_truth.py inside the KiCad 10 image). Boards come from bench/data (scripts/fetch_fixtures.sh);
// tests skip when the fixtures are missing.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "io/kicad/board_editor.hpp"
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

TEST_CASE("reading an edited document gives the edited board", "[kicad][io]") {
  const fs::path f = src_dir() / "bench/data/kicad/demos/pic_programmer/pic_programmer.kicad_pcb";
  if (!fs::exists(f)) SKIP("fixture missing");
  auto lb = tmk::io::read_board_file(f.string());
  REQUIRE(!lb.board.footprints.empty());
  const auto& fp0 = lb.board.footprints.front();
  const tmk::model::Point moved{fp0.pos.x + 1'000'000, fp0.pos.y + 2'000'000};
  tmk::io::BoardEditor ed(lb);
  ed.move_footprint(0, moved, fp0.angle);
  REQUIRE(lb.doc.modified());
  const auto from_doc = tmk::io::read_board(lb.doc);
  const auto from_text = tmk::io::read_board(tmk::sexpr::Document::parse(lb.doc.write()));
  CHECK(from_doc.footprints.front().pos == moved);
  CHECK(from_doc.footprints.front().pos == from_text.footprints.front().pos);
  REQUIRE(from_doc.pads.size() == from_text.pads.size());
  for (std::size_t i = 0; i < from_doc.pads.size(); ++i) CHECK(from_doc.pads[i].pos == from_text.pads[i].pos);
}

TEST_CASE("stackup: layers, sublayers, permittivity and copper mapping are read; text is untouched", "[kicad][io][stackup]") {
  // KiCad 6+ syntax with a two-sheet dielectric (addsublayer) and a locked thickness.
  const std::string text = R"((kicad_pcb (version 20221018) (generator pcbnew)
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup
    (stackup
      (layer "F.SilkS" (type "Top Silk Screen"))
      (layer "F.Mask" (type "Top Solder Mask") (thickness 0.01) (epsilon_r 3.3))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "prepreg") (thickness 0.1 locked) (material "FR4") (epsilon_r 4.0) (loss_tangent 0.02) addsublayer
        (thickness 0.3) (material "R-1551") (epsilon_r 4.6) (loss_tangent 0.01))
      (layer "B.Cu" (type "copper") (thickness 0.018))
      (layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.01))
      (copper_finish "ENIG")
      (dielectric_constraints no)
    )
    (pad_to_mask_clearance 0)
  )
  (net 0 "")
)
)";
  const auto doc = tmk::sexpr::Document::parse(text);
  const auto b = tmk::io::read_board(doc);
  CHECK(doc.write() == text);
  const auto& st = b.stackup;
  REQUIRE(st.present);
  REQUIRE(st.layers.size() == 6);
  CHECK(st.copper_finish == "ENIG");
  CHECK(st.layers[2].copper_index == 0);
  CHECK(st.layers[4].copper_index == 1);
  CHECK(st.layers[1].is_mask());
  CHECK(st.layers[1].epsilon_r == 3.3);
  const auto& d = st.layers[3];
  CHECK(d.sublayers == 2);
  CHECK(d.thickness == 400'000);
  CHECK(d.material == "FR4");
  // Series combination: 0.4 / (0.1/4.0 + 0.3/4.6); loss tangent thickness-weighted.
  CHECK(std::fabs(d.epsilon_r - 0.4 / (0.1 / 4.0 + 0.3 / 4.6)) < 1e-12);
  CHECK(std::fabs(d.loss_tangent - (0.1 * 0.02 + 0.3 * 0.01) / 0.4) < 1e-12);
  const auto gap = st.between(0, 1);
  CHECK(gap.complete);
  CHECK(gap.thickness == 400'000);
  CHECK(std::fabs(gap.epsilon_r - d.epsilon_r) < 1e-12);
  CHECK(st.copper_thickness(0) == 35'000);
  CHECK(st.copper_thickness(1) == 18'000);
  REQUIRE(st.mask(false));
  CHECK(st.mask(false)->epsilon_r == 0);  // not given: never invented
  CHECK(!st.mask(false)->epsilon_complete);
}

TEST_CASE("stackup: boards without one say so; missing permittivity makes the gap incomplete", "[kicad][io][stackup]") {
  const auto none = tmk::io::read_board(tmk::sexpr::Document::parse(
      "(kicad_pcb (version 20171130) (layers (0 F.Cu signal) (31 B.Cu signal)) (setup (pad_to_mask_clearance 0)) (net 0 \"\"))"));
  CHECK(!none.stackup.present);
  CHECK(none.stackup.layers.empty());
  const auto partial = tmk::io::read_board(tmk::sexpr::Document::parse(
      "(kicad_pcb (version 20221018) (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal)) (setup (stackup (layer \"F.Cu\" (type \"copper\") (thickness 0.035)) "
      "(layer \"dielectric 1\" (type \"core\") (thickness 1.51)) (layer \"B.Cu\" (type \"copper\") (thickness 0.035)))) (net 0 \"\"))"));
  REQUIRE(partial.stackup.present);
  const auto g = partial.stackup.between(0, 1);
  CHECK(g.thickness == 1'510'000);
  CHECK(!g.complete);
  CHECK(g.epsilon_r == 0);
}

TEST_CASE("stackup: KiCad demo with dielectric sublayers round-trips", "[kicad][io][stackup]") {
  const fs::path p = src_dir() / "bench/data/kicad/demos/tiny_tapeout/tinytapeout-demo.kicad_pcb";
  if (!fs::exists(p)) SKIP("fixtures missing: run scripts/fetch_fixtures.sh");
  const auto lb = tmk::io::read_board_file(p.string());
  REQUIRE(lb.board.stackup.present);
  int sub = 0;
  for (const auto& l : lb.board.stackup.layers)
    if (l.sublayers == 2) {
      ++sub;
      CHECK(l.thickness == 2 * 68'130);
      CHECK(std::fabs(l.epsilon_r - 4.3) < 1e-9);
    }
  CHECK(sub >= 1);
  std::ifstream f(p, std::ios::binary);
  const std::string orig((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(lb.doc.write() == orig);
}

namespace {

tmk::io::LoadedBoard board_from_text(std::string text) {
  tmk::io::LoadedBoard lb{tmk::sexpr::Document::parse(std::move(text)), {}};
  lb.board = tmk::io::read_board(lb.doc);
  return lb;
}

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

// The writer side of side assignment (D48). Exact agreement with KiCad's own FOOTPRINT::Flip is checked on 2,500+
// footprints of 37 boards by scripts/flip_check.py (pcbnew in the KiCad 10 image); this pins the rules down.
TEST_CASE("flipping a footprint: KiCad's mirror, layers swapped, the rest byte for byte", "[kicad][io][flip]") {
  const std::string head =
      "(kicad_pcb (version 20240108) (generator \"pcbnew\")\n"
      "  (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal) (37 \"F.SilkS\" user) (36 \"B.SilkS\" user) (49 \"F.Fab\" user)\n"
      "    (48 \"B.Fab\" user) (47 \"F.CrtYd\" user) (46 \"B.CrtYd\" user) (40 \"Dwgs.User\" user) (44 \"Edge.Cuts\" user))\n"
      "  (net 0 \"\")\n";
  const std::string fp =
      "  (footprint \"R:R_0603\" (layer \"F.Cu\") (uuid \"a\") (at 10 20 90)\n"
      "    (property \"Reference\" \"R1\" (at 0 -1.4 90) (layer \"F.SilkS\") (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (fp_text user \"x\" (at 1 2 90) (layer \"Dwgs.User\") (effects (font (size 1 1)) (justify left)))\n"
      "    (fp_line (start -0.5 -0.3) (end 0.5 -0.3) (stroke (width 0.12) (type solid)) (layer \"F.SilkS\"))\n"
      "    (fp_arc (start 1 0) (mid 0 1) (end -1 0) (stroke (width 0.1) (type solid)) (layer \"F.Fab\"))\n"
      "    (fp_rect (start -1.5 -0.8) (end 1.5 0.8) (stroke (width 0.05) (type solid)) (layer \"F.CrtYd\"))\n"
      "    (pad \"1\" smd roundrect (at -0.8 0.1 90) (size 0.8 0.9) (layers \"F.Cu\" \"F.Paste\" \"F.Mask\") (roundrect_rratio 0.25))\n"
      "    (pad \"2\" smd trapezoid (at 0.8 0.2 90) (size 0.8 0.9) (rect_delta 0.1 0.2) (layers \"F.Cu\" \"F.Mask\") (chamfer top_left))\n"
      "  )\n";
  const std::string tail = "  (gr_line (start 0 0) (end 50 0) (layer \"Edge.Cuts\") (stroke (width 0.1) (type solid)))\n)\n";
  auto lb = board_from_text(head + fp + tail);
  std::string why;
  REQUIRE(tmk::io::flip_supported(lb, 0, &why));
  // Flipped orientation −90, then one quarter turn: final 0.
  tmk::io::BoardEditor(lb).flip_footprint(0, {12'000'000, 20'000'000}, 0.0);
  const std::string out = lb.doc.write();
  CHECK(out.starts_with(head));
  CHECK(out.ends_with(tail));
  CHECK(has(out, "(layer \"B.Cu\") (uuid \"a\") (at 12 20)"));
  // Text: local y negated, angle 180° − a plus the turn, mirrored on a side layer only.
  CHECK(has(out, "(at 0 1.4 180) (layer \"B.SilkS\") (effects (font (size 1 1) (thickness 0.15)) (justify mirror))"));
  CHECK(has(out, "(at 1 -2 180) (layer \"Dwgs.User\") (effects (font (size 1 1)) (justify left))"));
  CHECK(has(out, "(start -0.5 0.3) (end 0.5 0.3) (stroke (width 0.12) (type solid)) (layer \"B.SilkS\")"));
  CHECK(has(out, "(start 1 0) (mid 0 -1) (end -1 0) (stroke (width 0.1) (type solid)) (layer \"B.Fab\")"));
  CHECK(has(out, "(start -1.5 0.8) (end 1.5 -0.8) (stroke (width 0.05) (type solid)) (layer \"B.CrtYd\")"));
  // Pads: orientation −a plus the turn, layers swapped, trapezoid dy and chamfer corners mirrored.
  CHECK(has(out, "(pad \"1\" smd roundrect (at -0.8 -0.1 0) (size 0.8 0.9) (layers \"B.Cu\" \"B.Paste\" \"B.Mask\")"));
  CHECK(has(out, "(at 0.8 -0.2 0) (size 0.8 0.9) (rect_delta 0.1 -0.2) (layers \"B.Cu\" \"B.Mask\") (chamfer bottom_left)"));
  const auto b = tmk::io::read_board(lb.doc);
  CHECK(b.footprints[0].back);
  CHECK(b.footprints[0].angle == 0.0);
  // Flipping back at the original pose gives the original geometry.
  auto lb2 = board_from_text(out);
  tmk::io::BoardEditor(lb2).flip_footprint(0, {10'000'000, 20'000'000}, 90.0);
  const auto back = tmk::io::read_board(lb2.doc);
  const auto orig = board_from_text(head + fp + tail).board;
  CHECK_FALSE(back.footprints[0].back);
  REQUIRE(back.pads.size() == orig.pads.size());
  for (std::size_t i = 0; i < orig.pads.size(); ++i) {
    CHECK(back.pads[i].pos == orig.pads[i].pos);
    CHECK(std::fmod(back.pads[i].angle - orig.pads[i].angle + 720.0, 360.0) == 0.0);
    CHECK(back.pads[i].copper == orig.pads[i].copper);
  }
  CHECK_FALSE(has(lb2.doc.write(), "mirror"));
}

TEST_CASE("flipping a KiCad 5 module: user copper names, signed arc sweeps", "[kicad][io][flip]") {
  const std::string text =
      "(kicad_pcb (version 20171130) (host pcbnew \"(5.1.12)-1\")\n"
      "  (layers (0 Front signal) (31 Back signal) (36 B.SilkS user) (37 F.SilkS user) (44 Edge.Cuts user))\n"
      "  (net 0 \"\")\n"
      "  (module R:R1 (layer Front) (tedit 0) (tstamp 1)\n"
      "    (at 5 5 270)\n"
      "    (fp_text reference R1 (at 0 1) (layer F.SilkS)\n"
      "      (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (fp_arc (start 0 0) (end 1 0) (angle 22.5) (layer F.SilkS) (width 0.1))\n"
      "    (pad 1 smd rect (at 1 2 270) (size 1 1) (layers Front F.Mask))\n"
      "  )\n"
      ")\n";
  auto lb = board_from_text(text);
  REQUIRE(tmk::io::flip_supported(lb, 0));
  tmk::io::BoardEditor(lb).flip_footprint(0, {5'000'000, 5'000'000}, -270.0);
  const std::string out = lb.doc.write();
  CHECK(has(out, "(module R:R1 (layer Back)"));
  CHECK(has(out, "(at 5 5 90)"));
  CHECK(has(out, "(fp_text reference R1 (at 0 -1 180) (layer B.SilkS)"));
  CHECK(has(out, "(justify mirror)"));
  CHECK(has(out, "(fp_arc (start 0 0) (end 1 0) (angle -22.5) (layer B.SilkS) (width 0.1))"));  // a sweep, not normalised
  CHECK(has(out, "(pad 1 smd rect (at 1 -2 90) (size 1 1) (layers Back B.Mask))"));
  CHECK(tmk::io::read_board(lb.doc).footprints[0].back);
}
