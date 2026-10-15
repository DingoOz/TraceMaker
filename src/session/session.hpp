// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A board session an agent can drive (design doc 19, milestone M18).
//
// The agent lists the open connections, asks the engine to route some nets with some hints, looks at the result, rips a net, goes back to an earlier
// state and saves. The engine is what it was: copper comes only from the router (every segment and via passes the exact check), a call is a
// transaction that is kept or not, hints are an advice file (doc 17) read by the same code, and the log of calls replays to the same bytes.
//
// One `Session::call` serves every transport (MCP and JSON lines, transport.hpp). It never throws: a refused call is {"error": {"code", "message"}}.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events.hpp"
#include "io/kicad/board_reader.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/router.hpp"

namespace tmk::session {

struct SessionOptions {
  std::string in;                 // the board; its project files supply the rules
  std::string out;                // where `save` writes; empty = `save` is refused
  std::uint64_t seed = 1;         // router seed of a `route` call that does not give one
  long work_limit = 0;            // search work of the whole session (expansions plus flood cells); 0 = none
  int gpu_device = -1;            // GPU for cost-to-go fields; -1 = CPU reference (same results)
  int threads = 4;                // portfolio threads of a `route` with several variants (the result does not depend on it)
  std::string log;                // JSON-lines log of the calls; empty = none
  events::Sink* sink = nullptr;   // the router's events (the viewer)
};

class Session {
 public:
  // Reads the board and its rules. Throws on an unreadable board.
  explicit Session(SessionOptions opt);

  // One tool call (doc 19 §4). Deterministic: the result depends on the board, the state, the arguments and the seed only.
  nlohmann::json call(const std::string& tool, const nlohmann::json& args);

  // The tools as MCP descriptors {name, description, inputSchema}, and the text the server sends as `instructions`.
  static nlohmann::json tools();
  static const char* instructions();

  // Runs the calls of a log written by `SessionOptions::log` and stops at the first result or state digest that differs.
  struct Replay {
    bool ok = false;
    int calls = 0;
    std::string message;
  };
  Replay replay(const std::string& log_path);

  int state() const { return cur_; }
  // FNV-1a 64 of the current state's session copper in canonical order (a digest, not a cryptographic hash).
  std::string state_digest() const;
  const model::Board& base() const { return lb_.board; }
  long work_used() const { return work_used_; }

 private:
  struct Copper {
    std::vector<model::Track> tracks;
    std::vector<model::Via> vias;
  };
  struct Reason {
    std::string reason, why;
    int rips = 0, fails = 0;
    std::vector<std::string> blockers;
  };
  struct OpenConn {
    std::string net, a, b;        // a, b: REF.PIN ("zone" for a plane)
    double length_mm = -1;        // -1: the connection ends in a plane
    int pad_a = -1, pad_b = -1;
  };
  struct State {
    int id = 0, parent = -1;
    std::string call;
    Copper copper;
    std::map<std::pair<std::string, std::string>, Reason> reasons;  // of the last route that made or left this state
    // Caches, filled on demand (a state never changes).
    std::optional<std::vector<OpenConn>> open[2];  // planned without / with soft zones (plane connections)
    std::optional<std::map<std::string, int>> errors[2];  // DRC errors by type, [0] on the stored fills, [1] with the zones refilled
    int unconnected[2] = {-1, -1};
  };
  struct LastRoute {
    std::shared_ptr<const model::Board> board;
    route::RouteResult result;
    int state = -1;
  };

  nlohmann::json dispatch(const std::string& tool, const nlohmann::json& args);
  nlohmann::json board_info();
  nlohmann::json open_connections(const nlohmann::json& args);
  nlohmann::json route(const nlohmann::json& args);
  nlohmann::json rip_net(const nlohmann::json& args);
  nlohmann::json check(const nlohmann::json& args);
  nlohmann::json stuck_report(const nlohmann::json& args);
  nlohmann::json added_examples(State& s, const std::map<std::string, int>& added, bool refill);
  nlohmann::json inspect_pad(const nlohmann::json& args);
  nlohmann::json inspect_net(const nlohmann::json& args);
  nlohmann::json history();
  nlohmann::json undo();
  nlohmann::json restore(const nlohmann::json& args);
  nlohmann::json save(const nlohmann::json& args);

  model::Board working(const Copper& c) const;               // the board with the session copper added and locked
  const std::vector<OpenConn>& open_of(State& s, bool soft_zones);  // plans the state's open connections (cached)
  const std::map<std::string, int>& errors_of(State& s, bool refill);  // DRC errors by type (cached); refill = judged as kicad-cli --refill-zones
  int add_state(State s);
  std::string digest_of(const Copper& c) const;
  nlohmann::json state_json(const State& s) const;

  SessionOptions opt_;
  io::LoadedBoard lb_;
  model::DesignRules rules_;
  std::string name_;
  std::vector<State> states_;
  int cur_ = 0;
  long work_used_ = 0;
  std::optional<LastRoute> last_;
  int calls_ = 0;
  bool soft_used_ = false;  // a route with soft zones was kept: the DRC then judges the board with its zones refilled
};

}  // namespace tmk::session
