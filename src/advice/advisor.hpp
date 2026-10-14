// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The advisor protocol (design doc 17 §5): an advisor is any executable that reads a stuck report (JSON) on stdin and writes
// an advice file (JSON) on stdout. The engine starts it through the shell, with a time limit, and treats whatever comes back
// as untrusted: the answer must be exactly one advice file and passes the same strict checks as a file written by hand.
// A missing, slow, failing or nonsensical advisor is a warning, never an error of the run (rule 5, doc 17 §6).
#include <string>

#include <nlohmann/json.hpp>

#include "advice/advice.hpp"

namespace tmk::advice {

struct ProcessResult {
  bool started = false;
  int exit_code = -1;          // -1: killed by a signal or never started
  bool timed_out = false;
  bool out_truncated = false;  // more output than the limit
  std::string out, err;
  double seconds = 0;
};
// Runs `command` with /bin/sh -c in its own process group, feeds it `input` on stdin, and collects stdout (at most
// `max_out` bytes) and stderr (at most `max_err`). After `timeout_s` seconds the whole group is killed.
ProcessResult run_process(const std::string& command, const std::string& input, double timeout_s, std::size_t max_out = std::size_t{1} << 20,
                          std::size_t max_err = std::size_t{1} << 16);

struct AdvisorOptions {
  std::string command;
  double timeout_s = 120;
};

struct Consulted {
  bool ok = false;
  Advice advice;               // aliases undone; names are not yet checked against the board
  std::string text;            // exactly what the advisor wrote (the file saved beside the output)
  std::string error;           // why there is no advice
  nlohmann::json summary;      // {"program", "seconds", "exit_code", "timed_out", "bytes_in", "bytes_out", "ok", "error"?, "stderr"?}
};
// Sends `report` to the advisor and reads its advice. The command line itself is not logged or put in the summary (it may carry
// credentials); only the program name is.
Consulted consult_advisor(const AdvisorOptions& opt, const nlohmann::json& report, const AliasMap& aliases, const std::string& advice_name);

}  // namespace tmk::advice
