// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/transport.hpp"

#include <string>

#include "core/version.hpp"

namespace tmk::session {
namespace {

using nlohmann::json;

// Reads one line of at most kMaxRequestBytes. A longer line is consumed and reported as too long.
bool read_line(std::istream& in, std::string& line, bool& too_long) {
  line.clear();
  too_long = false;
  char c;
  bool any = false;
  while (in.get(c)) {
    any = true;
    if (c == '\n') return true;
    if (line.size() < kMaxRequestBytes) line.push_back(c);
    else too_long = true;
  }
  return any;
}

json rpc_error(const json& id, int code, const std::string& message) { return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}; }

// Protocol revisions this server answers as itself; any other request gets the newest it knows.
bool known_protocol(const std::string& v) { return v == "2024-11-05" || v == "2025-03-26" || v == "2025-06-18"; }

}  // namespace

void run_mcp(Session& s, std::istream& in, std::ostream& out) {
  std::string line;
  bool too_long = false;
  auto send = [&](const json& j) { out << j.dump() << "\n" << std::flush; };
  while (read_line(in, line, too_long)) {
    if (line.empty() && !too_long) continue;
    if (too_long) {
      send(rpc_error(nullptr, -32600, "request line too long"));
      continue;
    }
    json req;
    try {
      req = json::parse(line);
    } catch (const std::exception&) {
      send(rpc_error(nullptr, -32700, "parse error"));
      continue;
    }
    if (!req.is_object() || !req.contains("method") || !req["method"].is_string()) {
      send(rpc_error(req.is_object() && req.contains("id") ? req["id"] : json(nullptr), -32600, "invalid request"));
      continue;
    }
    const std::string method = req["method"].get<std::string>();
    const bool notification = !req.contains("id");
    const json id = notification ? json(nullptr) : req["id"];
    if (method == "initialize") {
      std::string proto = "2025-06-18";
      if (req.contains("params") && req["params"].is_object() && req["params"].contains("protocolVersion") && req["params"]["protocolVersion"].is_string() &&
          known_protocol(req["params"]["protocolVersion"].get<std::string>()))
        proto = req["params"]["protocolVersion"].get<std::string>();
      send({{"jsonrpc", "2.0"},
            {"id", id},
            {"result",
             {{"protocolVersion", proto},
              {"capabilities", {{"tools", json::object()}}},
              {"serverInfo", {{"name", "tracemaker"}, {"version", std::string(tmk::version())}}},
              {"instructions", Session::instructions()}}}});
    } else if (method == "tools/list") {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", Session::tools()}}}});
    } else if (method == "tools/call") {
      const json params = req.contains("params") ? req["params"] : json::object();
      if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        send(rpc_error(id, -32602, "tools/call needs a tool name"));
        continue;
      }
      const json args = params.contains("arguments") ? params["arguments"] : json::object();
      const json result = s.call(params["name"].get<std::string>(), args);
      const bool is_error = result.is_object() && result.contains("error");
      send({{"jsonrpc", "2.0"},
            {"id", id},
            {"result", {{"content", json::array({{{"type", "text"}, {"text", result.dump()}}})}, {"structuredContent", result}, {"isError", is_error}}}});
    } else if (method == "ping") {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}});
    } else if (!notification) {
      send(rpc_error(id, -32601, "method not found: " + method));
    }  // a notification (notifications/initialized, ...) gets no answer
  }
}

void run_jsonl(Session& s, std::istream& in, std::ostream& out) {
  std::string line;
  bool too_long = false;
  auto send = [&](const json& j) { out << j.dump() << "\n" << std::flush; };
  while (read_line(in, line, too_long)) {
    if (line.empty() && !too_long) continue;
    if (too_long) {
      send({{"error", {{"code", "limit"}, {"message", "request line too long"}}}});
      continue;
    }
    json req;
    try {
      req = json::parse(line);
    } catch (const std::exception&) {
      send({{"error", {{"code", "bad_argument"}, {"message", "not JSON"}}}});
      continue;
    }
    if (!req.is_object() || !req.contains("tool") || !req["tool"].is_string()) {
      send({{"error", {{"code", "bad_argument"}, {"message", "expected {\"tool\": NAME, \"args\": {...}}"}}}});
      continue;
    }
    send(s.call(req["tool"].get<std::string>(), req.contains("args") ? req["args"] : json::object()));
  }
}

}  // namespace tmk::session
