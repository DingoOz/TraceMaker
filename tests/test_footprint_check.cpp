// SPDX-License-Identifier: GPL-3.0-or-later
// The footprint check against datasheet land patterns (issue #5, doc 05 section 43): a footprint that matches, one moved, turned, mirrored, resized,
// one pad short or over, a hole of the wrong size; a reference from a brand nobody trusts; how references are found; how they are loaded. The boards are
// KiCad text read by the real reader, so the properties, the side a footprint is on and its rotation come the way they do from a file.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "analysis/footprint_check.hpp"
#include "analysis/footprint_ref.hpp"
#include "io/kicad/board_reader.hpp"

namespace fs = std::filesystem;
using namespace tmk;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

struct PadSpec {
  std::string number;
  double x = 0, y = 0, w = 1, h = 1;
  std::string type = "smd", shape = "rect";
  double angle = 0;  // the pad's own angle in the footprint
  double drill = 0;
};

struct FpSpec {
  std::string lib = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm";
  std::string ref = "U1", value = "EX1234";
  double x = 50, y = 50, angle = 0;
  bool back = false;
  std::vector<std::pair<std::string, std::string>> props;
  std::vector<PadSpec> pads;
};

std::string num(double v) {
  std::ostringstream s;
  s << v;
  return s.str();
}

std::string board_text(const std::vector<FpSpec>& fps) {
  std::ostringstream o;
  o << "(kicad_pcb (version 20240108) (generator \"test\") (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal) (44 \"Edge.Cuts\" user))\n";
  for (const auto& f : fps) {
    const char* cu = f.back ? "B.Cu" : "F.Cu";
    o << "(footprint \"" << f.lib << "\" (layer \"" << cu << "\") (at " << num(f.x) << " " << num(f.y) << " " << num(f.angle) << ")\n";
    o << " (property \"Reference\" \"" << f.ref << "\") (property \"Value\" \"" << f.value << "\")\n";
    for (const auto& [k, v] : f.props) o << " (property \"" << k << "\" \"" << v << "\")\n";
    for (const auto& p : f.pads) {
      const std::string layers = p.type == "smd" ? std::string("(layers \"") + cu + "\" \"" + (f.back ? "B.Mask" : "F.Mask") + "\")" : "(layers \"*.Cu\" \"*.Mask\")";
      o << " (pad \"" << p.number << "\" " << p.type << " " << p.shape << " (at " << num(p.x) << " " << num(p.y) << " " << num(p.angle + f.angle) << ") (size " << num(p.w) << " " << num(p.h)
        << ")";
      if (p.drill > 0) o << " (drill " << num(p.drill) << ")";
      o << " " << layers << ")\n";
    }
    o << ")\n";
  }
  o << ")\n";
  return o.str();
}

model::Board read(const std::vector<FpSpec>& fps) { return io::read_board(sexpr::Document::parse(board_text(fps))); }

// An SOIC-8 land pattern as a datasheet draws it: pin 1 at the top left, pins down the left side, up the right.
std::vector<PadSpec> soic8() {
  std::vector<PadSpec> p;
  for (int i = 0; i < 4; ++i) p.push_back({std::to_string(i + 1), -2.7, -1.905 + 1.27 * i, 1.55, 0.6});
  for (int i = 0; i < 4; ++i) p.push_back({std::to_string(i + 5), 2.7, 1.905 - 1.27 * i, 1.55, 0.6});
  return p;
}

analysis::FootprintRef ref_of(const std::vector<PadSpec>& pads, const std::string& brand = "Example Semiconductor") {
  analysis::FootprintRef r;
  r.id = "test.json#EX1234";
  r.brand = brand;
  r.part = "EX1234";
  r.document = "EX1234 datasheet";
  r.revision = "C";
  r.page = 21;
  r.value = {"EX1234*"};
  for (const auto& p : pads) {
    analysis::RefPad rp;
    rp.number = p.number;
    rp.type = p.type == "thru_hole" ? model::PadType::ThruHole : p.type == "np_thru_hole" ? model::PadType::NpThruHole : model::PadType::Smd;
    rp.x = p.x * 1e6;
    rp.y = p.y * 1e6;
    rp.w = p.w * 1e6;
    rp.h = p.h * 1e6;
    rp.angle = p.angle;
    rp.drill_w = rp.drill_h = p.drill * 1e6;
    r.pads.push_back(rp);
  }
  return r;
}

analysis::RefLibrary library(std::vector<analysis::FootprintRef> refs, bool trust = true) {
  analysis::RefLibrary lib;
  lib.refs = std::move(refs);
  if (trust) lib.trusted = analysis::parse_trusted_brands("Example Semiconductor | Example Semi\n", "trusted_brands.txt");
  return lib;
}

analysis::FootprintResult check_one(const FpSpec& f, const analysis::RefLibrary& lib, analysis::FootprintCheckOptions opt = {}) {
  const auto b = read({f});
  const auto rep = analysis::check_footprints(b, lib, opt);
  REQUIRE(rep.results.size() == 1);
  return rep.results[0];
}

std::vector<std::string> kinds(const analysis::FootprintResult& r) {
  std::vector<std::string> k;
  for (const auto& i : r.issues) k.push_back(i.kind);
  return k;
}
bool has(const analysis::FootprintResult& r, const std::string& kind, const std::string& pad = "") {
  return std::any_of(r.issues.begin(), r.issues.end(), [&](const auto& i) { return i.kind == kind && (pad.empty() || i.pad == pad); });
}

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("tm_fpcheck_" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
  void write(const std::string& name, const std::string& text) const {
    fs::create_directories((path / name).parent_path());
    std::ofstream(path / name) << text;
  }
};

}  // namespace

TEST_CASE("footprint check: a footprint that is the datasheet's land pattern is ok, wherever and however it is placed", "[footprint_check]") {
  const auto lib = library({ref_of(soic8())});
  for (const double angle : {0.0, 90.0, 180.0, 270.0, 37.0}) {
    FpSpec f;
    f.pads = soic8();
    f.angle = angle;
    f.x = 71.3;
    f.y = 12.9;
    const auto r = check_one(f, lib);
    INFO("angle " << angle << " issues " << std::string(r.issues.empty() ? "" : r.issues[0].message));
    CHECK(r.status == analysis::FootprintStatus::Ok);
    CHECK(r.pads_matched == 8);
    CHECK(r.max_position_mm < 0.001);
    CHECK_FALSE(r.mirrored);
  }
}

TEST_CASE("footprint check: the library's origin is not part of the land pattern", "[footprint_check]") {
  // The same pattern with every pad moved by (0.3, -0.2): the footprint's origin is elsewhere, the pads are in the same places relative to each other.
  FpSpec f;
  f.pads = soic8();
  for (auto& p : f.pads) p.x += 0.3, p.y -= 0.2;
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Ok);
}

TEST_CASE("footprint check: the pattern may be turned a quarter turn in the library", "[footprint_check]") {
  // Pin 1 at the top left in the datasheet, at the bottom left in the library footprint (turned by 90 degrees, pads turned with it).
  FpSpec f;
  for (const auto& p : soic8()) {
    PadSpec q = p;
    q.x = p.y;
    q.y = -p.x;  // rot(p, 90) in KiCad's convention
    q.w = p.h;
    q.h = p.w;
    f.pads.push_back(q);
  }
  const auto r = check_one(f, library({ref_of(soic8())}));
  INFO(std::string(r.issues.empty() ? "" : r.issues[0].message));
  CHECK(r.status == analysis::FootprintStatus::Ok);
  CHECK(r.rotation == 90);
}

TEST_CASE("footprint check: one pad moved is called out with its number and by how much", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.pads[2].y += 0.15;  // pad 3
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  REQUIRE(kinds(r).size() == 1);  // the other seven pads are not blamed for it: the alignment does not follow one wrong pad
  CHECK(r.issues[0].kind == "position");
  CHECK(r.issues[0].pad == "3");
  CHECK_THAT(r.issues[0].actual_mm, WithinAbs(0.15, 0.001));
  CHECK(r.issues[0].severity == analysis::Severity::Error);
  CHECK(r.issues[0].message.find("pad 3") != std::string::npos);
  CHECK(r.issues[0].message.find("0.15 mm") != std::string::npos);
}

TEST_CASE("footprint check: a wrong pitch shows on the pads farthest from the centre", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  for (int i = 0; i < 4; ++i) {  // 1.35 instead of 1.27: pads 1 and 4 are 0.12 mm out, pads 2 and 3 0.04 mm
    f.pads[static_cast<std::size_t>(i)].y = -2.025 + 1.35 * i;
    f.pads[static_cast<std::size_t>(4 + i)].y = 2.025 - 1.35 * i;
  }
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "position", "1"));
  CHECK(has(r, "position", "4"));
  CHECK_FALSE(has(r, "position", "2"));  // 0.04 mm out: inside the tolerance
}

TEST_CASE("footprint check: size, drill and pad type differences are errors", "[footprint_check]") {
  auto pat = soic8();
  pat.push_back({"9", 0, 0, 2.4, 2.4});                                             // an exposed pad
  pat.push_back({"10", 4.5, 3, 1.6, 1.6, "thru_hole", "circle", 0, 1.0});           // a hole
  FpSpec f;
  f.pads = pat;
  f.pads[0].w = 1.8;                  // pad 1 wider
  f.pads[8].w = f.pads[8].h = 2.1;    // the exposed pad smaller
  f.pads[9].drill = 0.8;              // the hole smaller
  f.pads[1].type = "thru_hole";       // pad 2 plated through hole instead of SMD
  f.pads[1].drill = 0.8;
  const auto r = check_one(f, library({ref_of(pat)}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "size", "1"));
  CHECK(has(r, "size", "9"));
  CHECK(has(r, "drill", "10"));
  CHECK(has(r, "pad_type", "2"));
  CHECK_FALSE(has(r, "size", "3"));
}

TEST_CASE("footprint check: a pad turned a quarter turn has its width and height compared the other way", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  for (auto& p : f.pads) {  // the same outline described turned by 90 degrees: 0.6 wide and 1.55 high at 90 degrees
    p.angle = 90;
  }
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);  // turned pads are a different footprint ...
  CHECK(has(r, "size"));
  for (auto& p : f.pads) std::swap(p.w, p.h);  // ... unless the size is given the way the turned pad has it
  const auto r2 = check_one(f, library({ref_of(soic8())}));
  CHECK(r2.status == analysis::FootprintStatus::Ok);
}

TEST_CASE("footprint check: a missing pad, an extra pad and the count", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.pads.erase(f.pads.begin() + 3);  // no pad 4
  auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "pad_count"));
  CHECK(has(r, "missing_pad"));
  auto missing = std::find_if(r.issues.begin(), r.issues.end(), [](const auto& i) { return i.kind == "missing_pad"; });
  REQUIRE(missing != r.issues.end());
  CHECK(missing->message.find("pad 4") != std::string::npos);
  FpSpec g;
  g.pads = soic8();
  g.pads.push_back({"9", 0, 0, 2.4, 2.4});
  r = check_one(g, library({ref_of(soic8())}));
  CHECK(has(r, "pad_count"));
  CHECK(has(r, "extra_pad", "9"));
}

TEST_CASE("footprint check: a non-plated peg that the datasheet does not show is a warning, not an error", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.pads.push_back({"", 0, 0, 1.0, 1.0, "np_thru_hole", "circle", 0, 1.0});
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Ok);  // nothing to call out as an error
  REQUIRE(r.issues.size() == 1);
  CHECK(r.issues[0].kind == "extra_pad");
  CHECK(r.issues[0].severity == analysis::Severity::Warning);
}

TEST_CASE("footprint check: pin numbers that run the wrong way are a mirror", "[footprint_check]") {
  // Pins down the right side and up the left: the numbering of the same pattern mirrored.
  FpSpec f;
  f.pads = soic8();
  for (auto& p : f.pads) p.x = -p.x;
  auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "mirrored"));
  CHECK(r.mirrored);
  CHECK(r.pads_matched == 8);  // the pattern is recognised, with a clear name for what is wrong
  CHECK_FALSE(has(r, "position"));
  // A footprint on the back has its pads mirrored: that is what flipping does, and it is right.
  f.back = true;
  r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Ok);
  CHECK(r.mirrored);
  // On the back with the pads as drawn: it was not flipped.
  FpSpec g;
  g.pads = soic8();
  g.back = true;
  r = check_one(g, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "mirrored"));
}

TEST_CASE("footprint check: a pattern that is its own mirror image is never called mirrored", "[footprint_check]") {
  std::vector<PadSpec> two{{"1", -0.825, 0, 0.8, 0.95}, {"2", 0.825, 0, 0.8, 0.95}};
  for (const bool back : {false, true}) {
    FpSpec f;
    f.ref = "R1";
    f.value = "EX1234";
    f.pads = two;
    f.back = back;
    if (back)
      for (auto& p : f.pads) p.x = -p.x;  // flipped
    const auto r = check_one(f, library({ref_of(two)}));
    INFO("back " << back);
    CHECK(r.status == analysis::FootprintStatus::Ok);
  }
}

TEST_CASE("footprint check: a footprint whose pads are not the pattern's at all", "[footprint_check]") {
  FpSpec f;
  f.pads = {{"A", 0, 0, 1, 1}, {"B", 1, 0, 1, 1}};
  const auto r = check_one(f, library({ref_of(soic8())}));
  CHECK(r.status == analysis::FootprintStatus::Mismatch);
  CHECK(has(r, "pattern"));
}

TEST_CASE("footprint check: a reference from a brand that is not trusted gives no verdict", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.pads[2].y += 0.5;  // wrong, but nobody trusted the reference that says so
  auto r = check_one(f, library({ref_of(soic8(), "Some Random Site")}));
  CHECK(r.status == analysis::FootprintStatus::Untrusted);
  CHECK(r.brand == "Some Random Site");
  CHECK(has(r, "untrusted_source"));
  CHECK(r.issues[0].severity == analysis::Severity::Warning);
  CHECK(r.pads_matched == 0);  // not compared
  // The same reference is used when trust-all is given, and the report says so.
  auto lib = library({ref_of(soic8(), "Some Random Site")}, false);
  lib.trusted.trust_all = true;
  const auto b = read({f});
  const auto rep = analysis::check_footprints(b, lib, {});
  CHECK(rep.results[0].status == analysis::FootprintStatus::Mismatch);
  REQUIRE_FALSE(rep.notes.empty());
  CHECK(rep.notes[0].find("trust-all") != std::string::npos);
  // No list at all: nothing is trusted, and the report says what to do.
  const auto none = analysis::check_footprints(b, library({ref_of(soic8())}, false), {});
  CHECK(none.results[0].status == analysis::FootprintStatus::Untrusted);
  REQUIRE_FALSE(none.notes.empty());
  CHECK(none.notes[0].find("no trusted brand list") != std::string::npos);
}

TEST_CASE("footprint check: a trusted brand is found through its aliases and without corporate suffixes", "[footprint_check]") {
  const auto t = analysis::parse_trusted_brands("# datasheets we believe\nTexas Instruments | TI | Texas Instruments Inc.\nSTMicroelectronics\n\n  Analog Devices, Inc.   # inline comment\n", "x");
  CHECK(t.trusts("Texas Instruments Incorporated"));
  CHECK(t.trusts("ti"));
  CHECK(t.trusts("TEXAS INSTRUMENTS"));
  CHECK(t.trusts("STMicroelectronics N.V."));
  CHECK(t.trusts("Analog Devices"));
  CHECK_FALSE(t.trusts("Analog"));
  CHECK_FALSE(t.trusts("Microchip"));
  CHECK_FALSE(t.trusts(""));
  CHECK(analysis::normalise_brand("The Foo Corp.") == "foo");
}

TEST_CASE("footprint check: references are found by part number, library id, value or designator; the most specific wins", "[footprint_check]") {
  auto general = ref_of(soic8());
  general.id = "general";
  general.value = {};
  general.lib_id = {"*SOIC-8*"};
  auto specific = ref_of(soic8());
  specific.id = "specific";
  specific.value = {};
  specific.mpn = {"EX1234*"};
  specific.lib_id = {"*SOIC-8*"};
  auto other = ref_of(soic8());
  other.id = "other";
  other.value = {};
  other.mpn = {"NOPE*"};
  FpSpec f;
  f.pads = soic8();
  f.props = {{"Manufacturer_Part_Number", "ex1234abc"}};  // case does not matter for part numbers
  auto b = read({f});
  auto rep = analysis::check_footprints(b, library({general, specific, other}), {});
  REQUIRE(rep.results.size() == 1);
  CHECK(rep.results[0].ref_id == "specific");
  CHECK(rep.results[0].other_matches == 1);
  REQUIRE_FALSE(rep.notes.empty());
  CHECK(rep.notes.back().find("matches 2 references, used specific") != std::string::npos);
  // No part number on the footprint: the library id decides.
  f.props.clear();
  b = read({f});
  rep = analysis::check_footprints(b, library({general, specific, other}), {});
  CHECK(rep.results[0].ref_id == "general");
  // Designator and value selectors.
  auto by_ref = ref_of(soic8());
  by_ref.id = "by_ref";
  by_ref.value = {};
  by_ref.ref = {"U1?"};
  b = read({f});
  rep = analysis::check_footprints(b, library({by_ref}), {});
  CHECK(rep.results[0].status == analysis::FootprintStatus::Unverified);  // U1 is not U1?
  FpSpec g = f;
  g.ref = "U12";
  rep = analysis::check_footprints(read({g}), library({by_ref}), {});
  CHECK(rep.results[0].ref_id == "by_ref");
}

TEST_CASE("footprint check: a footprint without a reference is unverified, and an error where one is required", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.value = "OTHER";
  const auto lib = library({ref_of(soic8())});  // for EX1234*
  auto r = check_one(f, lib);
  CHECK(r.status == analysis::FootprintStatus::Unverified);
  CHECK(r.issues.empty());
  analysis::FootprintCheckOptions opt;
  opt.require = {"U*"};
  const auto b = read({f});
  const auto rep = analysis::check_footprints(b, lib, opt);
  CHECK(rep.results[0].status == analysis::FootprintStatus::Unverified);
  CHECK(has(rep.results[0], "no_reference"));
  CHECK(rep.errors == 1);
  CHECK(rep.unverified == 1);
}

TEST_CASE("footprint check: the designator filter, and footprints that are not parts", "[footprint_check]") {
  FpSpec u1;
  u1.pads = soic8();
  FpSpec u2 = u1;
  u2.ref = "U2";
  u2.pads[0].y += 0.3;
  FpSpec logo;
  logo.ref = "G1";
  const auto b = read({u1, u2, logo});  // the logo has no pads
  analysis::FootprintCheckOptions opt;
  auto rep = analysis::check_footprints(b, library({ref_of(soic8())}), opt);
  CHECK(rep.checked == 2);
  CHECK(rep.skipped == 1);
  CHECK(rep.ok == 1);
  CHECK(rep.mismatched == 1);
  opt.only = {"U1"};
  rep = analysis::check_footprints(b, library({ref_of(soic8())}), opt);
  CHECK(rep.checked == 1);
  CHECK(rep.mismatched == 0);
}

TEST_CASE("footprint check: tolerances: the run's, and a reference's own", "[footprint_check]") {
  FpSpec f;
  f.pads = soic8();
  f.pads[0].x += 0.08;
  auto lib = library({ref_of(soic8())});
  CHECK(check_one(f, lib).status == analysis::FootprintStatus::Mismatch);  // 0.08 > 0.05
  analysis::FootprintCheckOptions loose;
  loose.position_tol_mm = 0.1;
  CHECK(check_one(f, lib, loose).status == analysis::FootprintStatus::Ok);
  lib.refs[0].position_tol_nm = 0.02e6;  // the datasheet's own tolerance wins over the run's
  CHECK(check_one(f, lib, loose).status == analysis::FootprintStatus::Mismatch);
}

TEST_CASE("footprint check: the reference library loads from JSON with a footprint file, and refuses what it cannot take", "[footprint_check]") {
  TempDir d;
  // A footprint file as a manufacturer would give it.
  std::ostringstream mod;
  mod << "(footprint \"SOIC-8\" (version 20240108) (generator \"vendor\") (layer \"F.Cu\")\n";
  for (const auto& p : soic8()) mod << " (pad \"" << p.number << "\" smd roundrect (at " << p.x << " " << p.y << ") (size " << p.w << " " << p.h << ") (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n";
  mod << " (pad \"\" smd rect (at 0 0) (size 2 2) (layers \"F.Paste\")))\n";  // a paste aperture: not land pattern
  d.write("vendor/soic8.kicad_mod", mod.str());
  const json entry = {{"brand", "Example Semiconductor"}, {"part", "EX1234"}, {"package", "SOIC-8"},
                      {"datasheet", {{"document", "EX1234 datasheet"}, {"revision", "C"}, {"page", 21}, {"url", "https://example.invalid/ex1234.pdf"}}},
                      {"match", {{"mpn", {"EX1234*"}}, {"lib_id", "*SOIC-8*"}}},
                      {"footprint", "vendor/soic8.kicad_mod"},
                      {"tolerance_mm", {{"position", 0.03}}}};
  d.write("refs.json", json{{"format", "tracemaker-footprint-refs/1"}, {"references", {entry}}}.dump(1));
  d.write("trusted_brands.txt", "Example Semiconductor | Example Semi\n");
  const auto lib = analysis::load_references(d.path.string());
  REQUIRE(lib.refs.size() == 1);
  const auto& r = lib.refs[0];
  CHECK(r.brand == "Example Semiconductor");
  CHECK(r.page == 21);
  CHECK(r.pads.size() == 8);  // the paste aperture is not a pad of the pattern
  CHECK_THAT(r.position_tol_nm, WithinAbs(30000, 1e-6));
  CHECK(lib.trusted.trusts("Example Semi"));
  FpSpec f;
  f.pads = soic8();
  f.props = {{"MPN", "EX1234-TR"}};  // the reference is for this part number in this package
  CHECK(check_one(f, lib).status == analysis::FootprintStatus::Ok);
  f.props.clear();
  CHECK(check_one(f, lib).status == analysis::FootprintStatus::Unverified);  // a SOIC-8 of another part is not this one

  auto bad = [&](json e, const std::string& needle) {
    d.write("bad.json", json{{"format", "tracemaker-footprint-refs/1"}, {"references", {e}}}.dump());
    try {
      analysis::load_references((d.path / "bad.json").string());
      FAIL("expected a refusal: " << needle);
    } catch (const std::runtime_error& ex) {
      INFO(ex.what());
      CHECK(std::string(ex.what()).find(needle) != std::string::npos);
    }
  };
  json e = entry;
  e["tolerence_mm"] = 0.1;  // a misspelt key must not be ignored
  bad(e, "unknown key 'tolerence_mm'");
  e = entry;
  e.erase("datasheet");
  bad(e, "missing 'datasheet'");
  e = entry;
  e["match"] = json::object();
  bad(e, "selects nothing");
  e = entry;
  e["pads"] = json::array({{{"number", "1"}, {"at", {0, 0}}, {"size", {1, 1}}}});
  bad(e, "either 'pads' or 'footprint'");
  e = entry;
  e.erase("footprint");
  e["pads"] = json::array({{{"number", "1"}, {"type", "thru_hole"}, {"at", {0, 0}}, {"size", {1, 1}}}});
  bad(e, "needs a 'drill'");
  e = entry;
  e["footprint"] = "vendor/missing.kicad_mod";
  bad(e, "missing.kicad_mod");
  e = entry;
  e["match"] = {{"mpn", {""}}};
  bad(e, "empty pattern");
  d.write("bad.json", "{ not json");
  CHECK_THROWS_AS(analysis::load_references((d.path / "bad.json").string()), std::runtime_error);
  CHECK_THROWS_AS(analysis::load_references((d.path / "nothing").string()), std::runtime_error);
}

TEST_CASE("footprint check: inline pads in JSON give the same pattern as a footprint file", "[footprint_check]") {
  TempDir d;
  json pads = json::array();
  for (const auto& p : soic8()) pads.push_back({{"number", p.number}, {"at", {p.x, p.y}}, {"size", {p.w, p.h}}});
  const json entry = {{"brand", "Example Semiconductor"}, {"part", "EX1234"}, {"datasheet", {{"document", "EX1234 datasheet"}}}, {"match", {{"value", "EX1234"}}}, {"pads", pads}};
  d.write("a.json", json{{"format", "tracemaker-footprint-refs/1"}, {"references", {entry}}}.dump());
  auto lib = analysis::load_references((d.path / "a.json").string(), "");
  lib.trusted = analysis::parse_trusted_brands("Example Semiconductor", "inline");
  FpSpec f;
  f.pads = soic8();
  CHECK(check_one(f, lib).status == analysis::FootprintStatus::Ok);
  f.pads[7].x += 0.2;
  CHECK(check_one(f, lib).status == analysis::FootprintStatus::Mismatch);
}

TEST_CASE("footprint check: the report is the same every time, and says what it found in JSON, text and a viewer message", "[footprint_check]") {
  FpSpec good, bad;
  good.pads = soic8();
  bad.ref = "U2";
  bad.pads = soic8();
  bad.pads[2].y += 0.15;
  bad.x = 80;
  const auto b = read({good, bad});
  const auto lib = library({ref_of(soic8())});
  const auto rep = analysis::check_footprints(b, lib, {});
  const auto rep2 = analysis::check_footprints(b, lib, {});
  const json j = analysis::footprint_check_json(b, rep);
  CHECK(j.dump() == analysis::footprint_check_json(b, rep2).dump());
  CHECK(j["summary"]["checked"] == 2);
  CHECK(j["summary"]["ok"] == 1);
  CHECK(j["summary"]["mismatch"] == 1);
  CHECK(j["summary"]["errors"] == 1);
  CHECK(j["footprints"][1]["status"] == "mismatch");
  CHECK(j["footprints"][1]["reference"]["brand"] == "Example Semiconductor");
  CHECK(j["footprints"][1]["reference"]["page"] == 21);
  CHECK(j["footprints"][1]["issues"][0]["kind"] == "position");
  CHECK(j["footprints"][1]["issues"][0]["pad"] == "3");
  CHECK(j["footprints"][1]["issues"][0]["label"] == "pad 3: 0.15 mm off");  // a few words for a mark on the board
  CHECK(j["footprints"][0]["status"] == "ok");
  const auto lines = analysis::footprint_check_lines(rep, false);
  REQUIRE(lines.size() >= 3);
  CHECK(lines[0].find("U2") == 0);
  CHECK(lines[0].find("MISMATCH vs Example Semiconductor EX1234 (EX1234 datasheet rev C, p.21)") != std::string::npos);
  CHECK(lines[1].find("error") != std::string::npos);
  CHECK(lines[1].find("pad 3") != std::string::npos);
  CHECK(lines.back().find("2 footprints checked against 1 references: 1 ok, 1 mismatch") != std::string::npos);
  CHECK(analysis::footprint_check_lines(rep, true).size() == lines.size() + 1);  // --all adds the one that is fine
  const json m = json::parse(analysis::footprint_check_message(rep));
  CHECK(m["type"] == "footprint_check");
  CHECK(m["errors"] == 1);
  REQUIRE(m["footprints"].size() == 1);  // only what has something to say
  CHECK(m["footprints"][0]["ref"] == "U2");
  CHECK(m["footprints"][0]["p"][0] == 80'000'000);
  CHECK(m["footprints"][0]["issues"][0]["kind"] == "position");
  CHECK(m["footprints"][0]["issues"][0]["label"] == "pad 3: 0.15 mm off");
}

TEST_CASE("footprint check: a footprint on the back is compared with the datasheet as the flipped front", "[footprint_check]") {
  // A 3-pad pattern with no symmetry, on the front and on the back (pads mirrored in x, which is how KiCad stores a flipped footprint).
  std::vector<PadSpec> pat{{"1", -1.0, -1.0, 0.8, 0.5}, {"2", -1.0, 1.0, 0.8, 0.5}, {"3", 1.2, 0.0, 1.0, 1.4}};
  const auto lib = library({ref_of(pat)});
  FpSpec front, back;
  front.pads = pat;
  back.pads = pat;
  back.back = true;
  back.angle = 90;
  for (auto& p : back.pads) p.x = -p.x;
  const auto b = read({front, back});
  auto rep = analysis::check_footprints(b, lib, {});
  REQUIRE(rep.results.size() == 2);
  CHECK(rep.results[0].status == analysis::FootprintStatus::Ok);
  CHECK(rep.results[1].status == analysis::FootprintStatus::Ok);
  CHECK(rep.results[1].mirrored);
  CHECK_FALSE(rep.results[0].mirrored);
}
