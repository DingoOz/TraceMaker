// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Live status of this process for the progress site's live page (devsite /live, D165): what the process is doing, written as
// JSON to <dir>/<pid>.json about once a second by a background thread (written to a temporary file and renamed, so a reader
// never sees half a file). <dir> is $TM_LIVE_DIR, else $XDG_CACHE_HOME/tracemaker/live, else ~/.cache/tracemaker/live.
// On by default; TM_LIVE=0 turns it off. A process that ends within its first second writes nothing.
//
// Write-only: the engine never reads any of it back, so it cannot change a result (rule 2). The router updates its slot with
// relaxed atomic stores, which cost next to nothing next to a search; everything else takes a mutex a few times a second.
#include <atomic>
#include <string>

namespace tmk::live {

// One router's progress (a portfolio variant, by name), updated lock-free from the thread running it.
struct Slot {
  std::atomic<int> routed{0}, total{0};
  std::atomic<long> work{0}, budget{0};  // search expansions of the run in progress and its work budget (0: none)
  std::atomic<long> work_done{0};        // search expansions of the slot's finished runs
  std::atomic<int> phase{0};             // 0 waiting, 1 setup, 2 first pass, 3 negotiation, 4 clean-up, 5 done
  std::atomic<int> runs{0};              // router runs started in this slot (one per round)
};

// From main(): starts the writer thread, unless TM_LIVE=0. argv is recorded as the command line.
void start(int argc, char** argv);
// From main(): writes the final state with the exit code and stops the writer. Without it (std::exit) the last state says "ended".
void finish(int exit_code);
// True after start() when the status is being written.
bool enabled();
// The slot named `name`, created on first use; the pointer stays valid for the life of the process. nullptr when not enabled.
Slot* slot(const std::string& name);
// Sets one line of the status ("stage", "board", "round", ...), shown in the order first set. An empty value removes it.
void field(const std::string& key, const std::string& value);
// Adds a line to the log tail (the last 200 lines are kept).
void log(const std::string& line);
// printf to stderr and the log tail.
void eprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tmk::live
