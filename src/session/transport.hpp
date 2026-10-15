// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The two ways to talk to a Session (design doc 19 §6). Both call Session::call, so they cannot differ.
//
//   MCP over standard input and output: JSON-RPC 2.0, one message per line, `initialize`, `tools/list`, `tools/call`, `ping`, and the
//   `notifications/initialized` notification. Any MCP client can be the agent. Standard output carries protocol messages only.
//   JSON lines: one request {"tool": NAME, "args": {...}} per line, one response per line (the tool's result, or {"error": ...}).
#include <istream>
#include <ostream>

#include "session/session.hpp"

namespace tmk::session {

inline constexpr std::size_t kMaxRequestBytes = 1 << 20;  // a line longer than this is refused (the rest of the line is skipped)

// Return when the input ends. Never throw for a bad request: it is answered.
void run_mcp(Session& s, std::istream& in, std::ostream& out);
void run_jsonl(Session& s, std::istream& in, std::ostream& out);

}  // namespace tmk::session
