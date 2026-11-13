// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The session daemon (`tracemaker daemon`, design doc 19 §10, milestone M18): board sessions (session.hpp) that outlive the process that
// asked for them, so a client that runs once per user action — the KiCad plugin — keeps the board read, the rules, the states and their
// caches (open connections, DRC errors) between actions.
//
// Transport: a Unix-domain stream socket that only its owner can open (the directory is 0700, the socket 0600), JSON lines, one request
// per line and one answer per line. Clients are served one at a time, in the order they connect: a session is one board and one user,
// and the router already uses the cores.
//
//   {"op": "status"}                                    -> {"sessions": [...], "version"}
//   {"op": "open", "board": PATH, "key": K, "fingerprint": F, "out": PATH, "work_limit": N, "seed": S}
//        -> {"session": ID, "reused": bool, "state"}. With `key` (the client's name for the board, e.g. the path KiCad has open) and a
//           `fingerprint` equal to the one recorded for that session's current state (`mark`), the session is reused and `board` is not
//           read; otherwise a new session is started from a copy of `board` and its project files (the client may delete them).
//   {"op": "call", "session": ID, "tool": T, "args": {...}}   -> {"result": ...}  (Session::call, doc 19 §4)
//   {"op": "diff", "session": ID, "from": STATE}             -> Session::copper_diff
//   {"op": "mark", "session": ID, "fingerprint": F}          -> records F for the current state (what the client's board now looks like)
//   {"op": "close", "session": ID}, {"op": "shutdown"}
//
// A refused request is {"error": {"code", "message"}}. The fingerprint is the client's: the daemon only compares strings. A client must
// change it whenever anything the router reads changes (copper, footprints, zones, rules); a wrong match would route on a stale board,
// which is why every route is still checked against the session's own board and the client commits only the copper the daemon returns.
#include <string>

namespace tmk::session {

struct DaemonOptions {
  std::string socket;        // socket path; empty = default_socket_path()
  int idle_exit_s = 1800;    // exit after this long without a client (0 = never)
  int max_sessions = 4;      // the least recently used session is closed beyond this
  int threads = 4;           // SessionOptions::threads
  bool use_gpu = false;
};

// $XDG_RUNTIME_DIR/tracemaker/daemon.sock, else /tmp/tracemaker-<uid>/daemon.sock.
std::string default_socket_path();

// Serves until `shutdown` or the idle limit. Returns 0, or 1 when the socket cannot be created (another daemon holds it: the message says so).
int run_daemon(const DaemonOptions& opt);

}  // namespace tmk::session
