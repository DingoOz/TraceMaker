#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "io/kicad/project_reader.hpp"

namespace fs = std::filesystem;
using tmk::io::parse_length;
using tmk::io::read_design_rules;

TEST_CASE("lengths with units convert to nanometres", "[rules]") {
  CHECK(parse_length("0.2mm") == 200'000);
  CHECK(parse_length("0.2") == 200'000);
  CHECK(parse_length("8mil") == 203'200);
  CHECK(parse_length("0.01in") == 254'000);
  CHECK(parse_length("150um") == 150'000);
  CHECK_FALSE(parse_length("abc").has_value());
}

TEST_CASE("net-class wildcard patterns", "[rules]") {
  using tmk::model::wildcard_match;
  CHECK(wildcard_match("+12V", "+12V"));
  CHECK(wildcard_match("*USB*", "/usb/USB_D+"));
  CHECK(wildcard_match("85Ohm-diff*", "85Ohm-diff_DP"));
  CHECK(wildcard_match("D?", "D1"));
  CHECK_FALSE(wildcard_match("D?", "D10"));
  CHECK_FALSE(wildcard_match("*USB*", "/usb/D+"));
}

TEST_CASE("project net classes, patterns and minimums are read", "[rules]") {
  const fs::path b = fs::path(TM_SOURCE_DIR) / "bench/data/kicad/demos/video/video.kicad_pcb";
  if (!fs::exists(b)) SKIP("fixtures missing");
  const auto r = read_design_rules(b.string());
  CHECK(r.warnings.empty());
  REQUIRE(r.classes.size() == 2);
  CHECK(r.default_class().name == "Default");
  CHECK(r.default_class().clearance == 200'000);
  CHECK(r.default_class().via_diameter == 889'000);
  CHECK(r.class_for("+12V").name == "pwr");
  CHECK(r.class_for("+12V").track_width == 250'000);
  CHECK(r.class_for("SOMETHING").name == "Default");
  CHECK(r.minimums.track_width == 200'000);
  CHECK(r.minimums.hole_to_hole == 250'000);
  CHECK(r.minimums.copper_edge_clearance == 10'000);
}

TEST_CASE("classes inherit null values from Default; explicit assignments and custom rules are read", "[rules]") {
  const fs::path b = fs::path(TM_SOURCE_DIR) / "bench/data/kicad/demos/jetson-agx-thor-baseboard/jetson-agx-thor-baseboard.kicad_pcb";
  if (!fs::exists(b)) SKIP("fixtures missing");
  const auto r = read_design_rules(b.string());
  CHECK(r.warnings.empty());
  const auto* dp = r.find_class("85Ohm-diff_DP");
  REQUIRE(dp != nullptr);
  CHECK(dp->clearance == r.default_class().clearance);
  CHECK(dp->priority == 1);
  CHECK(r.class_for("/CSI/CAM0.CSI0_CLK+").name == "90Ohm-diff_CSI");
  REQUIRE(r.custom.size() == 5);
  CHECK(r.custom[0].layer == "inner");
  CHECK(r.custom[0].condition == "(A.NetClass == '85Ohm-diff*')");
  REQUIRE(r.custom[0].constraints.size() == 2);
  CHECK(r.custom[0].constraints[0].type == "track_width");
  CHECK(r.custom[0].constraints[0].opt == 150'000);
}
