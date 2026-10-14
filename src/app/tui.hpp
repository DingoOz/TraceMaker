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

// Where a value comes from, lowest precedence first. `run` is what belongs to one run (board, output) and is never
// written to a file; `global` is the user's options for every board, `project` the options of the board's folder and
// `file` a file named with --config. A higher scope overrides a lower one.
enum class Scope { run = 0, global = 1, project = 2, file = 3 };
inline constexpr int kScopes = 4;
const char* scope_name(Scope s);  // "this run", "global", "project", "file"

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
  std::string value;                   // what is in effect: value option: its text; flag: the chosen spelling; empty: the default
  std::string layer[kScopes];          // the same, per scope; `value` is the highest scope's that is not empty
  Scope scope = Scope::run;            // the scope `value` comes from (meaningless while value is empty)
  bool is_flag() const { return !spellings.empty(); }
};

// Keys beyond the printable characters, which stand for themselves.
namespace key {
inline constexpr int ctrl_c = 3, tab = 9, enter = 13, ctrl_u = 21, esc = 27, backspace = 127;
inline constexpr int up = 0x1000, down = 0x1001, page_up = 0x1002, page_down = 0x1003, home = 0x1004, end = 0x1005, del = 0x1006, shift_tab = 0x1007;
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

// How a piece of text is painted. The terminal layer maps these to colours (or to bold, dim and reverse under
// NO_COLOR); the model only says what the text is.
enum class Style {
  plain, frame, title, heading, dim, label, key, code, ok, warn, error, accent,
  global, project, file, run,          // a value or a name of that scope
  tag_global, tag_project, tag_file, tag_run  // the same as a badge
};
struct Span {
  std::string text;
  Style style = Style::plain;
};
// One screen line: `text` is the plain text (what a test or a monochrome terminal sees), `spans` the same in pieces
// with their styles. `highlight` paints the whole line as the current row.
struct Line {
  std::string text;
  std::vector<Span> spans;
  bool highlight = false;
};
// Columns the text takes (UTF-8 characters count one each, which is right for the box-drawing characters).
std::size_t display_width(const std::string& s);

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

  // The options file of a scope: one `name = value` per line, as `route --config` reads it. Only the values that
  // belong to that scope are written; what a lower or higher scope sets stays in its own file.
  std::string config_text(Scope s) const;
  std::string config_text() const { return config_text(target_); }
  // Takes the values of such a file into a scope; returns one message per line it could not use.
  std::vector<std::string> load_config(const std::string& text, Scope s = Scope::file);

  // Makes a scope available as a place to keep options: `path` is its file, `exists` whether that file is there.
  // `file` is available from the start (the --config file); the caller enables `global` and `project`.
  void enable_scope(Scope s, std::string path, bool exists);
  void disable_scope(Scope s);
  // The scope that edits go to and that `s` saves; Tab moves it.
  Scope target() const { return target_; }
  void set_target(Scope s);
  bool dirty(Scope s) const { return dirty_[static_cast<int>(s)]; }
  // Records that the target scope's file was written.
  void mark_saved();

  // Sets a field by its label or its argument spelling (e.g. "board", "--output"); false if there is none.
  bool set(const std::string& name, const std::string& value);

  // The file of the target scope.
  const std::string& save_path() const { return path_[static_cast<int>(target_)]; }
  void set_save_path(std::string p) { path_[static_cast<int>(Scope::file)] = std::move(p); }
  void set_status(std::string s) { status_ = std::move(s); }
  const std::string& status() const { return status_; }
  const std::string& error() const { return error_; }
  const std::vector<Field>& fields() const { return fields_; }

 private:
  enum class Mode { list, edit, filter, save };
  std::vector<std::size_t> visible() const;
  void recheck();
  bool edit_key(int k);
  Scope home(const Field& f) const { return f.save ? target_ : Scope::run; }  // the scope an edit of `f` goes to
  void refresh(Field& f) const;                                               // recompute f.value and f.scope
  void put(Field& f, std::string value);                                      // an edit: sets the layer of home(f)
  void next_target(int step);

  std::string command_;
  std::vector<Field> fields_;
  Check check_;
  Mode mode_ = Mode::list;
  std::size_t sel_ = 0;          // index into visible()
  mutable std::size_t top_ = 0;  // first visible row on screen; follows the selection when rendering
  bool show_hidden_ = false;
  std::string filter_, buffer_, status_, error_;
  Scope target_ = Scope::file;
  bool enabled_[kScopes] = {false, false, false, true};
  bool exists_[kScopes] = {false, false, false, false};
  bool dirty_[kScopes] = {false, false, false, false};
  std::string path_[kScopes] = {"", "", "", "tracemaker-route.toml"};
};

// Writes the target scope's options to its file (creating the folder) and reports the outcome in the status line.
void save_options(Model& m);
// Runs the model on the controlling terminal (/dev/tty, so standard output stays free for `print`) until the user
// quits, runs or prints. Options files are written here. Throws std::runtime_error without a terminal.
Action run_terminal(Model& m);
// The same for a scripted key sequence (tests): characters stand for themselves, and <up> <down> <pgup> <pgdn> <home>
// <end> <enter> <space> <esc> <bs> <del> <lt> name the others. Ends with `print` when the keys run out.
Action run_keys(Model& m, const std::string& keys);

}  // namespace tmk::app::tui
