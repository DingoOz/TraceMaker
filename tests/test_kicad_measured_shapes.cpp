// SPDX-License-Identifier: GPL-3.0-or-later
// Shapes KiCad's DRC measures that the router must not underestimate: copper text widths, custom-pad primitives and Margin-layer strokes.
// The numbers come from kicad-cli 10.0.6 plots and DRC runs of the PCBench boards named in the comments.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "drc/copper.hpp"
#include "io/kicad/board_reader.hpp"
#include "route/obstacles.hpp"
#include "sexpr/sexpr.hpp"

using namespace tmk;
namespace {
constexpr Coord M = 1'000'000;

model::Board read(const std::string& body) {
  const std::string text =
      "(kicad_pcb (version 20240108) (generator \"pcbnew\")\n(general (thickness 1.6))\n(paper \"A4\")\n"
      "(layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal) (44 \"Edge.Cuts\" user) (45 \"Margin\" user))\n"
      "(setup (pad_to_mask_clearance 0.2))\n(net 0 \"\")\n(net 1 \"A\")\n" + body + ")\n";
  return io::read_board(sexpr::Document::parse(text));
}
}  // namespace

TEST_CASE("Copper text boxes cover KiCad's stroke-font advances", "[route][text]") {
  // domotics_base-board-arranged: 'Main Board     -       08/2016', size 1.5 on B.Cu, centred: KiCad plots it from x 81.04 to 118.89 around x = 100.
  auto b = read("(gr_text \"Main Board     -       08/2016\" (at 100 50) (layer \"B.Cu\") (effects (font (size 1.5 1.5) (thickness 0.3)) (justify mirror)))\n");
  model::DesignRules rules;
  route::Obstacles obs(b, rules);
  REQUIRE(obs.copper_texts().size() == 1);
  const auto& box = obs.copper_texts()[0].second.box;
  CHECK(box.x0 <= 81'040'000);
  CHECK(box.x1 >= 118'890'000);
  CHECK(box.x1 - box.x0 < 45 * M);  // not wildly larger than the text
}

TEST_CASE("Custom pad primitives grow by half their stroke width and include circles and lines", "[drc][pad]") {
  // flatpack2-adapter_EltekFlatpack2 JC3 pad 2: gr_poly (width 0.1) is drawn 0.05 mm bigger than its points.
  auto b = read(
      "(footprint \"F\" (layer \"F.Cu\") (at 10 10)\n"
      "  (pad \"1\" thru_hole custom (at 0 0) (size 1 1) (drill 0.5) (layers \"*.Cu\" \"*.Mask\") (net 1 \"A\")\n"
      "    (options (clearance outline) (anchor circle))\n"
      "    (primitives (gr_poly (pts (xy 0 0) (xy 2 0) (xy 2 2) (xy 0 2)) (width 0.1))\n"
      "                (gr_circle (center 5 0) (end 6 0) (width 0.2))\n"
      "                (gr_line (start 0 -3) (end 0 -4) (width 0.4)))))\n");
  REQUIRE(b.pads.size() == 1);
  const auto shapes = drc::pad_shapes(b.pads[0]);
  geom::Box all;
  for (const auto& s : shapes) all.add(s.box);
  CHECK(all.x1 >= 10 * M + 6'100'000);   // circle: centre 5 + radius 1 + half the 0.2 pen
  CHECK(all.y1 >= 10 * M + 2'050'000);   // polygon edge at 2 + 0.05
  CHECK(all.y0 <= 10 * M - 4'200'000);   // line end at -4 minus half its 0.4 pen
}

TEST_CASE("Margin-layer lines keep their stroke as the edge clearance obstacle", "[drc][edge]") {
  // RaspberryPi-PoE_PoELLi_PI: Margin lines of width 2 mm; KiCad's copper_edge_clearance is 0 for copper that touches the stroke.
  auto b = read("(gr_line (start 0 0) (end 20 0) (layer \"Margin\") (width 2))\n");
  const auto cm = drc::build_copper(b);
  REQUIRE(cm.margins.size() == 1);
  CHECK(cm.margins[0].r == M);
}
