// SPDX-License-Identifier: GPL-3.0-or-later
// The options screen's model (doc 02 §2.1, D89): keys in, command line, options file and screen text out.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "app/tui.hpp"

using namespace tmk::app::tui;

namespace {

std::vector<Field> sample() {
  std::vector<Field> f(7);
  f[0].label = "board", f[0].save = false, f[0].help = "Board file";
  f[1].label = "-o, --output", f[1].arg = "--output", f[1].key = "output", f[1].save = false;
  f[2].label = "--via-cost-mm", f[2].arg = "--via-cost-mm", f[2].key = "via-cost-mm", f[2].def = "3", f[2].help = "Cost of a via";
  f[3].label = "--soft-zones", f[3].spellings = {"--soft-zones"}, f[3].help = "Zone fills do not block";
  f[4].label = "--pair-twists / --no-pair-twists", f[4].spellings = {"--pair-twists", "--no-pair-twists"};
  f[5].label = "--first-nets", f[5].arg = "--first-nets", f[5].key = "first-nets", f[5].help = "Nets routed first (see also via)";
  f[6].label = "--max-expansions", f[6].arg = "--max-expansions", f[6].key = "max-expansions", f[6].hidden = true;
  return f;
}

std::string screen(const Model& m, int rows = 24, int cols = 100) {
  std::string s;
  for (const Line& l : m.render(rows, cols)) s += l.text + "\n";
  return s;
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("tui: keys set values and flags; the command line follows", "[tui]") {
  Model m("tracemaker route", sample());
  REQUIRE(m.command_line() == "tracemaker route");
  // board: Enter opens the editor, Enter keeps the text.
  REQUIRE(run_keys(m, "<enter>my board.kicad_pcb<enter>") == Action::print);
  REQUIRE(m.args() == std::vector<std::string>{"my board.kicad_pcb"});
  // Down twice to --via-cost-mm; a negative value is passed as --name=value so it is not read as an option.
  run_keys(m, "<down><down><enter>-2<enter>");
  // The single flag toggles, the two-spelling flag cycles: default, first, second, default.
  run_keys(m, "<down><space><down><space><space>");
  REQUIRE(m.args() == std::vector<std::string>{"my board.kicad_pcb", "--via-cost-mm=-2", "--soft-zones", "--no-pair-twists"});
  REQUIRE(m.command_line() == "tracemaker route 'my board.kicad_pcb' --via-cost-mm=-2 --soft-zones --no-pair-twists");
  run_keys(m, "<space>");
  REQUIRE(m.fields()[4].value.empty());
  // `d` puts an option back to its default; Esc leaves an edit without keeping it.
  run_keys(m, "<up>d<up><enter>99<esc>");
  REQUIRE(m.args() == std::vector<std::string>{"my board.kicad_pcb", "--via-cost-mm=-2"});
  // Editing: backspace removes a whole UTF-8 character, Ctrl-U the line.
  run_keys(m, "<home><enter>" + std::string(1, static_cast<char>(key::ctrl_u)) + "a\xc3\xa9<bs>b<enter>");
  REQUIRE(m.fields()[0].value == "ab");
}

TEST_CASE("tui: the filter narrows the list, names before descriptions; hidden options appear on request", "[tui]") {
  Model m("tracemaker route", sample());
  REQUIRE(!has(screen(m), "--max-expansions"));
  REQUIRE(has(screen(m), "1 experimental options hidden"));
  run_keys(m, "h");
  REQUIRE(has(screen(m), "--max-expansions"));
  run_keys(m, "h/via<enter>");
  const std::string s = screen(m);
  REQUIRE(has(s, "--via-cost-mm"));
  REQUIRE(has(s, "--first-nets"));  // its description mentions "via"
  REQUIRE(!has(s, "--soft-zones"));
  REQUIRE(s.find("--via-cost-mm") < s.find("--first-nets"));
  // The selection is the first match: Enter edits --via-cost-mm.
  run_keys(m, "<enter>7<enter><esc>");
  REQUIRE(m.fields()[2].value == "7");
  REQUIRE(has(screen(m), "--soft-zones"));  // Esc cleared the filter and did not quit
  // A hidden option that is set stays listed.
  REQUIRE(m.load_config("max-expansions = 5\n").empty());
  REQUIRE(has(screen(m), "--max-expansions"));
  run_keys(m, "/zzz<enter>");
  REQUIRE(has(screen(m), "no option matches"));
}

TEST_CASE("tui: the options file round-trips and leaves out what belongs to one run", "[tui]") {
  Model m("tracemaker route", sample());
  m.set("board", "b.kicad_pcb");
  m.set("--output", "o.kicad_pcb");
  m.set("--via-cost-mm", "5.5");
  m.set("--first-nets", "XTAL \"1\",C:\\x");
  run_keys(m, "<down><down><down><space><down><space><space>");
  const std::string text = m.config_text();
  REQUIRE(has(text, "via-cost-mm = 5.5\n"));
  REQUIRE(has(text, "soft-zones = true\n"));
  REQUIRE(has(text, "no-pair-twists = true\n"));
  REQUIRE(has(text, "first-nets = \"XTAL \\\"1\\\",C:\\\\x\"\n"));
  REQUIRE(!has(text, "kicad_pcb"));

  Model n("tracemaker route", sample());
  REQUIRE(n.load_config(text).empty());
  n.set("board", "b.kicad_pcb");
  n.set("--output", "o.kicad_pcb");
  REQUIRE(n.args() == m.args());

  // Hand-written files: dashes, bare names, arrays, single quotes, false for the other spelling, comments.
  Model h("tracemaker route", sample());
  const auto problems = h.load_config("# c\n; c\n--soft-zones\npair-twists = false\nfirst-nets = [\"A\", 'B,C' ,D]\n"
                                      "via-cost-mm='4'\nnope = 1\nsoft-zones = maybe\n");
  REQUIRE(h.args() == std::vector<std::string>{"--via-cost-mm", "4", "--soft-zones", "--no-pair-twists", "--first-nets", "A,B,C,D"});
  REQUIRE(problems.size() == 2);
  REQUIRE(has(problems[0], "line 7: no option named nope"));
  REQUIRE(has(problems[1], "line 8: soft-zones is a flag"));
  // A single-spelling flag set to false is the default.
  REQUIRE(h.load_config("soft-zones = off\n").empty());
  REQUIRE(h.fields()[3].value.empty());
}

TEST_CASE("tui: the parser's verdict is shown and gates the run", "[tui]") {
  int calls = 0;
  Model m("tracemaker route", sample(), [&](const std::vector<std::string>& args) {
    ++calls;
    return std::find(args.begin(), args.end(), "--output") == args.end() ? std::string("--output is required") : std::string();
  });
  REQUIRE(calls == 1);
  REQUIRE(has(screen(m), "not runnable: --output is required"));
  REQUIRE(run_keys(m, "r") == Action::print);  // refused: the keys ran out instead
  REQUIRE(has(screen(m), "cannot run yet: --output is required"));
  REQUIRE(run_keys(m, "<down><enter>out.kicad_pcb<enter>r") == Action::run);
  REQUIRE(m.error().empty());
  REQUIRE(has(screen(m), "ready to run"));
  REQUIRE(run_keys(m, "q") == Action::quit);
  REQUIRE(run_keys(m, "p") == Action::print);
  // The save prompt asks for the path; Esc cancels, Enter hands the path to the caller.
  REQUIRE(m.press('s') == Action::none);
  REQUIRE(has(screen(m), "save options to: tracemaker-route.toml_"));
  REQUIRE(m.press(key::esc) == Action::none);
  m.press('s');
  m.press(key::ctrl_u);
  for (char c : std::string("x.toml")) m.press(c);
  REQUIRE(m.press(key::enter) == Action::save);
  REQUIRE(m.save_path() == "x.toml");
}

TEST_CASE("tui: the screen fits any size and keeps the selection in view", "[tui]") {
  std::vector<Field> many;
  for (int i = 0; i < 60; ++i) {
    Field f;
    f.label = "--option-" + std::to_string(i);
    f.spellings = {f.label};
    f.help = std::string(300, 'h');
    many.push_back(f);
  }
  Model m("tracemaker route", many);
  for (const auto& [rows, cols] : std::vector<std::pair<int, int>>{{24, 80}, {10, 40}, {3, 20}, {1, 5}, {50, 200}}) {
    const auto lines = m.render(rows, cols);
    REQUIRE(static_cast<int>(lines.size()) == rows);
    for (const Line& l : lines) REQUIRE(static_cast<int>(l.text.size()) <= std::max(cols, 20));
  }
  run_keys(m, "<end>");
  REQUIRE(has(screen(m), "> --option-59"));
  run_keys(m, "<pgup><pgup>");
  REQUIRE(has(screen(m), "> --option-39"));
  run_keys(m, "<home><down>");
  REQUIRE(has(screen(m, 12, 60), "> --option-1 "));
  // A long command line is cut with "..." instead of pushing the key line off the screen.
  for (int i = 0; i < 60; ++i) run_keys(m, "<space><down>");
  const auto lines = m.render(24, 80);
  REQUIRE(has(lines[21].text, "..."));
  REQUIRE(has(lines[23].text, "q quit"));
}
