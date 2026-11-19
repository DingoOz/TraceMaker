// SPDX-License-Identifier: GPL-3.0-or-later
// `tracemaker tui` jobs screen (doc 02 §2.2, D193): the route runs of this user, read from the live status files every
// run writes (core/live.hpp), with what each has left to do, the options it was given, and what to change on a rerun.
//
// Like the options screen, `JobsModel` is free of terminal code: jobs and keys go in, lines of text come out. The
// advice is a pure function of one job (`advise`), so it is tested on recorded jobs.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/tui.hpp"

namespace tmk::app::tui {

// One router of a job (a portfolio variant), as its live file shows it.
struct JobSlot {
  std::string name;
  int routed = 0, total = 0, phase = 0, runs = 0;
};

// One `route` process, from its live file and, once it has finished, the summary it wrote with --json.
struct Job {
  std::string file;  // the live file
  int pid = 0;
  std::string state;  // running, done (exit code known), ended (left without one), gone (the file says running, the process is not there)
  int exit_code = -1;
  double started = 0, updated = 0, elapsed_s = 0, cpu_s = 0;
  std::vector<std::string> argv;  // the arguments after `tracemaker route`
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> fields;  // stage, round, result ... in the order the run set them
  std::vector<JobSlot> slots;
  std::vector<int> h_routed, h_total;  // samples every h_step_s seconds
  int h_step_s = 1;
  std::vector<std::string> log;
  nlohmann::json summary;  // null until the run has written it

  std::string field(const std::string& key) const;     // "" if not set
  std::optional<std::string> option(const std::string& spelling) const;  // a value option's value; nullopt if not given
  bool has(const std::string& spelling) const;          // a flag or option given
  std::string path(const std::string& p) const;         // p against the job's folder
  std::string board() const;                            // the input board as given
  int routed() const;                                   // best so far (the summary's once finished)
  int total() const;                                    // connections, 0 while unknown
  double budget_s() const;                              // the wall-time limit read from the "budget" field, 0 if none
  double stalled_s() const;                             // seconds since the best routed count last rose (from the history)
};

// Reads one live file's JSON. `alive(pid, proc_start)` says whether that process still runs (it reads /proc in use).
Job parse_job(const nlohmann::json& j, const std::function<bool(int, long)>& alive);
// The route jobs of the live folder, running ones first, then newest first: those that run or changed in the last
// `max_age_s` seconds, at most `limit`.
std::vector<Job> read_jobs(const std::string& dir, double max_age_s, std::size_t limit);
std::string live_folder();  // as core/live.cpp chooses it

// What to try next for a job, with the data it rests on. `set` are the options a rerun would take (a flag has an
// empty value), `unset` those it would drop. Advice with neither is information only.
struct Advice {
  std::string text, why;
  std::vector<std::pair<std::string, std::string>> set;
  std::vector<std::string> unset;
};
std::vector<Advice> advise(const Job& j);

// The jobs screen.
class JobsModel {
 public:
  using Loader = std::function<std::vector<Job>(bool all)>;  // all: a week of jobs, else running and the last day
  explicit JobsModel(Loader load);

  void reload();
  Action press(int k);
  std::vector<Line> render(int rows, int cols) const;

  const Job* selected() const;
  // After Action::edit: the advice chosen with it (1-9), or nullptr for the job's options unchanged.
  const Advice* chosen() const { return chosen_ < 0 ? nullptr : &advice_[static_cast<std::size_t>(chosen_)]; }
  void set_status(std::string s) { status_ = std::move(s); }
  double now = 0;  // seconds since the epoch; the caller sets it before rendering (tests fix it)

 private:
  void select_changed();
  Loader load_;
  std::vector<Job> jobs_;
  std::vector<Advice> advice_;
  std::size_t sel_ = 0;
  mutable std::size_t top_ = 0;
  int chosen_ = -1;
  bool all_ = false;
  std::string status_;
};

// Runs the jobs screen on the terminal, reading the jobs again every two seconds, until a key leaves it (quit,
// back to the options, or edit).
Action run_terminal(JobsModel& m);
Action run_keys(JobsModel& m, const std::string& keys);

}  // namespace tmk::app::tui
