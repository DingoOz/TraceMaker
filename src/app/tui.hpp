// SPDX-License-Identifier: GPL-3.0-or-later
// `tracemaker tui`: choose the options of a command on a terminal screen (doc 02 §2.1, D89).
//
// The screen is a list of the command's options, taken from the command-line definitions themselves, so it cannot
// fall behind them. `Model` holds the state and is free of terminal code: keys go in, lines of text come out. The
// terminal layer below it only reads keys and paints those lines.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tmk::app::tui {

// One command-line option as the screen shows it.
struct Field {
  std::string label;                   // as shown: "--via-cost-mm", "-o, --output", "board"
  std::string arg;                     // spelling written before the value; empty for a positional argument
  std::string key;                     // options-file key of a value option: its long name without the dashes
  std::vector<std::string> spellings;  // a flag's spellings ("--pair-twists", "--no-pair-twists"); empty for a value option
  std::string help;
  std::string def;                     // the default as text; empty if not known
  bool hidden = false;                 // experimental and test options, shown on request
  bool save = true;                    // false for what belongs to one run (board, output): not written to an options file
  std::string value;                   // value option: its text; flag: the chosen spelling; empty: left at the default
  bool is_flag() const { return !spellings.empty(); }
};

// Keys beyond the printable characters, which stand for themselves.
namespace key {
inline constexpr int ctrl_c = 3, enter = 13, ctrl_u = 21, esc = 27, backspace = 127;
inline constexpr int up = 0x1000, down = 0x1001, page_up = 0x1002, page_down = 0x1003, home = 0x1004, end = 0x1005, del = 0x1006;
}

// One line of an options file: `name = value`. Leading dashes on the name, quotes around the value and a TOML
// array ([a, b] becomes a,b) are accepted; a name alone means `name = true`. `#` and `;` start a comment line.
struct Entry {
  int line = 0;
  std::string name, value;
};
std::vector<Entry> parse_options(const std::string& text);
// A flag's value in an options file: 1 for true/on/yes/1, 0 for false/off/no/0, -1 for anything else.
int truth(const std::string& value);

enum class Action { none, quit, run, print, save };

struct Line {
  enum Style { plain, title, selected, set, dim, error };
  std::string text;
  Style style = plain;
};

class Model {
 public:
  // Says what is wrong with a list of arguments (the command line's own parser); empty if nothing is.
  using Check = std::function<std::string(const std::vector<std::string>&)>;

  Model(std::string command, std::vector<Field> fields, Check check = {});

  // One key. `save` asks the caller to write config_text() to save_path(); `run` is only returned when the check
  // passes.
  Action press(int k);
  // The screen as `rows` lines of at most `cols` characters.
  std::vector<Line> render(int rows, int cols) const;

  std::vector<std::string> args() const;  // the chosen arguments: positionals first, then options in list order
  std::string command_line() const;       // the command with those arguments, quoted for a shell

  // The options file: one `name = value` per line, as `route --config` reads it.
  std::string config_text() const;
  // Takes the values of such a file; returns one message per line it could not use.
  std::vector<std::string> load_config(const std::string& text);

  // Sets a field by its label or its argument spelling (e.g. "board", "--output"); false if there is none.
  bool set(const std::string& name, const std::string& value);

  const std::string& save_path() const { return save_path_; }
  void set_save_path(std::string p) { save_path_ = std::move(p); }
  void set_status(std::string s) { status_ = std::move(s); }
  const std::string& status() const { return status_; }
  const std::string& error() const { return error_; }
  const std::vector<Field>& fields() const { return fields_; }

 private:
  enum class Mode { list, edit, filter, save };
  std::vector<std::size_t> visible() const;
  void recheck();
  bool edit_key(int k);

  std::string command_;
  std::vector<Field> fields_;
  Check check_;
  Mode mode_ = Mode::list;
  std::size_t sel_ = 0;          // index into visible()
  mutable std::size_t top_ = 0;  // first visible row on screen; follows the selection when rendering
  bool show_hidden_ = false;
  std::string filter_, buffer_, save_path_ = "tracemaker-route.toml", status_, error_;
};

// Writes config_text() to save_path() and reports the outcome in the status line.
void save_options(Model& m);
// Runs the model on the controlling terminal (/dev/tty, so standard output stays free for `print`) until the user
// quits, runs or prints. Options files are written here. Throws std::runtime_error without a terminal.
Action run_terminal(Model& m);
// The same for a scripted key sequence (tests): characters stand for themselves, and <up> <down> <pgup> <pgdn> <home>
// <end> <enter> <space> <esc> <bs> <del> <lt> name the others. Ends with `print` when the keys run out.
Action run_keys(Model& m, const std::string& keys);

}  // namespace tmk::app::tui
