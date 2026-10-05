// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "sexpr/sexpr.hpp"

using tmk::sexpr::Document;
using tmk::sexpr::format_mm;
using tmk::sexpr::parse_mm;

TEST_CASE("millimetre strings convert to nanometres exactly", "[sexpr]") {
  CHECK(parse_mm("0") == 0);
  CHECK(parse_mm("141.605") == 141'605'000);
  CHECK(parse_mm("-5.08") == -5'080'000);
  CHECK(parse_mm("0.000001") == 1);
  CHECK(parse_mm("0.0000005") == 1);   // rounds half away from zero
  CHECK(parse_mm("-0.0000005") == -1);
  CHECK(parse_mm("12") == 12'000'000);
  CHECK(parse_mm("1e-3") == 1'000);
  CHECK_FALSE(parse_mm("abc").has_value());
  CHECK_FALSE(parse_mm("1.2.3").has_value());
  CHECK(format_mm(141'605'000) == "141.605");
  CHECK(format_mm(-5'080'000) == "-5.08");
  CHECK(format_mm(12'000'000) == "12");
  CHECK(format_mm(1) == "0.000001");
  CHECK(format_mm(-1) == "-0.000001");
  CHECK(format_mm(0) == "0");
}

TEST_CASE("documents parse into a navigable tree", "[sexpr]") {
  const auto d = Document::parse("(kicad_pcb\n\t(version 20260206)\n\t(net \"a \\\"q\\\"\")\n\t(segment (start 1 2) (end 3.5 4))\n)\n");
  const auto root = d.root();
  CHECK(d.head(root) == "kicad_pcb");
  const auto v = d.find(root, "version");
  REQUIRE(v != tmk::sexpr::kNoNode);
  CHECK(d.number_at(v, 1) == 20260206.0);
  CHECK(d.str_at(d.find(root, "net"), 1) == "a \"q\"");
  const auto seg = d.find(root, "segment");
  CHECK(d.nm_at(d.find(seg, "end"), 1) == 3'500'000);
  CHECK(d.find_all(root, "segment").size() == 1);
}

TEST_CASE("unmodified documents write back byte for byte", "[sexpr]") {
  const std::string src = "(a\n  (b 1 2)   ; not a comment, just a symbol\n\t(c \"x\\ny\")\n)\n\n";
  const auto d = Document::parse(src);
  CHECK(d.write() == src);
}

TEST_CASE("malformed documents raise ParseError with a line number", "[sexpr]") {
  CHECK_THROWS_AS(Document::parse("(a (b)"), tmk::sexpr::ParseError);
  CHECK_THROWS_AS(Document::parse("(a))"), tmk::sexpr::ParseError);
  CHECK_THROWS_AS(Document::parse("(a \"open)"), tmk::sexpr::ParseError);
  CHECK_THROWS_AS(Document::parse(""), tmk::sexpr::ParseError);
}

TEST_CASE("edits remove, replace and append with KiCad indentation", "[sexpr]") {
  const std::string src = "(kicad_pcb\n\t(version 1)\n\t(segment\n\t\t(start 0 0)\n\t)\n\t(via (at 1 1))\n)\n";
  auto d = Document::parse(src);
  const auto root = d.root();
  d.remove(d.find(root, "segment"));
  d.replace(d.find(root, "version"), "(version 2)");
  d.append_child(root, "(segment\n\t(start 5 5)\n)");
  CHECK(d.write() == "(kicad_pcb\n\t(version 2)\n\t(via (at 1 1))\n\t(segment\n\t\t(start 5 5)\n\t)\n)\n");
}

TEST_CASE("append to an inline list moves the closing paren to its own line", "[sexpr]") {
  auto d = Document::parse("(a\n\t(b (c 1))\n)");
  d.append_child(d.find(d.root(), "b"), "(d 2)");
  CHECK(d.write() == "(a\n\t(b (c 1)\n\t\t(d 2)\n\t)\n)");
}

TEST_CASE("replacing the same node twice keeps the last replacement", "[sexpr]") {
  auto d = Document::parse("(a (b 1))");
  const auto b = d.find(d.root(), "b");
  d.replace(b, "(b 2)");
  d.replace(b, "(b 3)");
  CHECK(d.write() == "(a (b 3))");
}
