// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "io/srj/srj.hpp"

namespace tmk::io {
using nlohmann::json;

std::string srj_layer_name(int z, int layer_count) {
  if (z <= 0) return "top";
  if (z >= layer_count - 1) return "bottom";
  return "inner" + std::to_string(z);
}

// tscircuit's mapLayerNameToZ: top = 0, bottom = layerCount - 1, innerN = N.
int srj_layer_index(const std::string& name, int layer_count) {
  if (name == "top") return 0;
  if (name == "bottom") return layer_count - 1;
  if (name.rfind("inner", 0) != 0 || name.size() == 5) return -1;
  int z = 0;
  for (std::size_t i = 5; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9' || z > 100) return -1;
    z = z * 10 + (name[i] - '0');
  }
  return z >= 1 && z < layer_count - 1 ? z : -1;
}

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("SRJ: " + what); }

double number(const json& j, const char* key, const std::string& where) {
  if (!j.contains(key) || !j[key].is_number()) fail(where + ": missing number '" + key + "'");
  const double v = j[key].get<double>();
  if (!std::isfinite(v)) fail(where + ": '" + key + "' is not finite");
  return v;
}

std::optional<double> opt_number(const json& j, const char* key) {
  if (!j.contains(key) || !j[key].is_number()) return std::nullopt;
  const double v = j[key].get<double>();
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

std::string opt_string(const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); }
bool flag(const json& j, const char* key) { return j.contains(key) && j[key].is_boolean() && j[key].get<bool>(); }

std::vector<int> layer_list(const json& names, int layer_count, const std::string& where) {
  std::vector<int> out;
  if (!names.is_array()) fail(where + ": 'layers' is not an array");
  for (const auto& n : names) {
    if (!n.is_string()) fail(where + ": layer name is not a string");
    const int z = srj_layer_index(n.get<std::string>(), layer_count);
    if (z < 0) fail(where + ": unknown layer '" + n.get<std::string>() + "' for layerCount " + std::to_string(layer_count));
    out.push_back(z);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Union-find over the id strings of tscircuit's connectivity map. Ids are numbered in order of first use, so
// nothing depends on a hash order.
struct Groups {
  std::map<std::string, int> index;
  std::vector<int> parent;
  int id(const std::string& s) {
    const auto [it, fresh] = index.emplace(s, static_cast<int>(parent.size()));
    if (fresh) parent.push_back(it->second);
    return it->second;
  }
  int find(int x) {
    while (parent[static_cast<std::size_t>(x)] != x) x = parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
    return x;
  }
  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
  }
};

// The connectivity map's position key: centre rounded to 0.01 mm (JavaScript Math.round: half towards +inf) and
// the sorted stack indices.
std::string point_hash(double x, double y, const std::vector<int>& layers) {
  std::string s = std::to_string(static_cast<long long>(std::floor(x * 100 + 0.5))) + "," + std::to_string(static_cast<long long>(std::floor(y * 100 + 0.5))) + ":";
  for (std::size_t i = 0; i < layers.size(); ++i) s += (i ? "-" : "") + std::to_string(layers[i]);
  return s;
}

}  // namespace

SrjProblem read_srj(const std::string& text) {
  json d;
  try {
    d = json::parse(text);
  } catch (const json::exception& e) {
    fail(std::string("not valid JSON: ") + e.what());
  }
  if (!d.is_object()) fail("the document is not an object");
  SrjProblem p;
  const double lc = number(d, "layerCount", "problem");
  if (lc < 1 || lc > 32 || lc != std::floor(lc)) fail("layerCount must be an integer from 1 to 32");
  p.layer_count = static_cast<int>(lc);
  auto& r = p.rules;
  r.min_trace_width = number(d, "minTraceWidth", "problem");
  if (r.min_trace_width <= 0) fail("minTraceWidth must be positive");
  r.nominal_trace_width = opt_number(d, "nominalTraceWidth");
  // tscircuit's getViaDimensions: the explicit snake_case and camelCase fields first, the deprecated single
  // diameter last.
  for (const char* k : {"min_via_hole_diameter", "minViaHoleDiameter"})
    if (!r.via_hole) r.via_hole = opt_number(d, k);
  for (const char* k : {"min_via_pad_diameter", "minViaPadDiameter", "minViaDiameter"})
    if (!r.via_pad) r.via_pad = opt_number(d, k);
  r.obstacle_margin = opt_number(d, "defaultObstacleMargin");
  r.trace_to_pad = opt_number(d, "minTraceToPadEdgeClearance");
  r.trace_to_hole = opt_number(d, "minTraceToHoleEdgeClearance");
  r.board_edge = opt_number(d, "minBoardEdgeClearance");
  r.via_to_pad = opt_number(d, "minViaEdgeToPadEdgeClearance");
  r.via_hole_to_via_hole = opt_number(d, "minViaHoleEdgeToViaHoleEdgeClearance");
  r.drill_to_drill = opt_number(d, "minPlatedHoleDrillEdgeToDrillEdgeClearance");
  r.pad_to_pad = opt_number(d, "minPadEdgeToPadEdgeClearance");
  r.blind_buried_vias = flag(d, "allowBlindAndBuriedVias");
  r.via_in_pad = flag(d, "allowViaInPad");
  for (const auto& [name, v] : std::initializer_list<std::pair<const char*, std::optional<double>>>{
           {"nominalTraceWidth", r.nominal_trace_width}, {"via hole diameter", r.via_hole}, {"via pad diameter", r.via_pad}})
    if (v && *v <= 0) fail(std::string(name) + " must be positive");
  for (const auto& [name, v] : std::initializer_list<std::pair<const char*, std::optional<double>>>{
           {"defaultObstacleMargin", r.obstacle_margin}, {"minTraceToPadEdgeClearance", r.trace_to_pad},
           {"minTraceToHoleEdgeClearance", r.trace_to_hole}, {"minBoardEdgeClearance", r.board_edge},
           {"minViaEdgeToPadEdgeClearance", r.via_to_pad}, {"minViaHoleEdgeToViaHoleEdgeClearance", r.via_hole_to_via_hole},
           {"minPlatedHoleDrillEdgeToDrillEdgeClearance", r.drill_to_drill}, {"minPadEdgeToPadEdgeClearance", r.pad_to_pad}})
    if (v && *v < 0) fail(std::string(name) + " must not be negative");

  if (!d.contains("bounds") || !d["bounds"].is_object()) fail("missing 'bounds'");
  p.min_x = number(d["bounds"], "minX", "bounds");
  p.max_x = number(d["bounds"], "maxX", "bounds");
  p.min_y = number(d["bounds"], "minY", "bounds");
  p.max_y = number(d["bounds"], "maxY", "bounds");
  if (p.max_x <= p.min_x || p.max_y <= p.min_y) fail("bounds are empty");
  if (d.contains("outline") && d["outline"].is_array()) {
    for (const auto& q : d["outline"]) p.outline.emplace_back(number(q, "x", "outline"), number(q, "y", "outline"));
    if (!p.outline.empty() && p.outline.size() < 3) fail("outline has fewer than three points");
  }

  if (!d.contains("obstacles") || !d["obstacles"].is_array()) fail("missing 'obstacles'");
  if (!d.contains("connections") || !d["connections"].is_array()) fail("missing 'connections'");
  Groups g;
  int assignable = 0, off_board = 0, terminal_vias = 0;
  for (const auto& c : d["connections"]) {
    const std::string where = "connection " + std::to_string(p.connections.size());
    SrjConnection k;
    k.name = opt_string(c, "name");
    if (k.name.empty()) fail(where + ": missing 'name'");
    k.width = opt_number(c, "nominalTraceWidth");
    if (!k.width) k.width = opt_number(c, "width");
    if (k.width && *k.width <= 0) fail(where + ": trace width must be positive");
    off_board += flag(c, "isOffBoard");
    if (!c.contains("pointsToConnect") || !c["pointsToConnect"].is_array()) fail(where + ": missing 'pointsToConnect'");
    const int self = g.id(k.name);
    if (c.contains("__rootConnectionNames") && c["__rootConnectionNames"].is_array())
      for (const auto& n : c["__rootConnectionNames"])
        if (n.is_string()) g.unite(self, g.id(n.get<std::string>()));
    if (const std::string n = opt_string(c, "__netConnectionName"); !n.empty()) g.unite(self, g.id(n));
    for (const auto& q : c["pointsToConnect"]) {
      SrjPoint pt;
      pt.x = number(q, "x", where);
      pt.y = number(q, "y", where);
      const bool one = q.contains("layer"), many = q.contains("layers");
      if (one == many) fail(where + ": a point needs exactly one of 'layer' and 'layers'");
      pt.layers = layer_list(one ? json::array({q["layer"]}) : q["layers"], p.layer_count, where);
      if (pt.layers.empty()) fail(where + ": a point has no layer");
      pt.point_id = opt_string(q, "pointId");
      pt.pcb_port_id = opt_string(q, "pcb_port_id");
      terminal_vias += q.contains("terminalVia");
      g.unite(self, g.id(point_hash(pt.x, pt.y, pt.layers)));
      if (!pt.pcb_port_id.empty()) g.unite(self, g.id(pt.pcb_port_id));
      if (!pt.point_id.empty()) g.unite(self, g.id(pt.point_id));
      k.points.push_back(std::move(pt));
    }
    p.connections.push_back(std::move(k));
  }
  std::vector<int> obstacle_group;
  for (const auto& o : d["obstacles"]) {
    const std::string where = "obstacle " + std::to_string(p.obstacles.size());
    SrjObstacle k;
    if (opt_string(o, "type") != "rect") fail(where + ": unknown type '" + opt_string(o, "type") + "'");
    if (const std::string s = opt_string(o, "shape"); s == "circle") k.circle = true;
    else if (!s.empty()) fail(where + ": unknown shape '" + s + "'");
    k.id = opt_string(o, "obstacleId");
    k.component = opt_string(o, "componentId");
    if (!o.contains("center") || !o["center"].is_object()) fail(where + ": missing 'center'");
    k.x = number(o["center"], "x", where);
    k.y = number(o["center"], "y", where);
    k.width = number(o, "width", where);
    k.height = k.circle ? k.width : number(o, "height", where);
    if (k.width <= 0 || k.height <= 0) fail(where + ": size must be positive");
    k.rotation = opt_number(o, "ccwRotationDegrees").value_or(0.0);
    k.non_plated_hole = flag(o, "isNonPlatedHole");
    k.copper_pour = flag(o, "isCopperPour");
    assignable += flag(o, "netIsAssignable");
    if (!o.contains("layers")) fail(where + ": missing 'layers'");
    k.layers = layer_list(o["layers"], p.layer_count, where);
    if (k.layers.empty()) fail(where + ": no layer");
    for (const char* key : {"connectedTo", "offBoardConnectsTo"})
      if (o.contains(key) && o[key].is_array())
        for (const auto& n : o[key])
          if (n.is_string() && !n.get<std::string>().empty()) k.connected_to.push_back(n.get<std::string>());
    if (k.non_plated_hole && !k.connected_to.empty()) fail(where + ": a non-plated hole cannot be connected");
    // An obstacle with an empty connectedTo is a keep-out and joins nothing, not even an obstacle at the same
    // centre (the connectivity map would join those by position; two unconnected keep-outs have no net to share).
    int self = -1;
    if (!k.connected_to.empty()) {
      self = g.id(point_hash(k.x, k.y, k.layers));
      if (!k.id.empty()) g.unite(self, g.id(k.id));
      for (const auto& n : k.connected_to) g.unite(self, g.id(n));
    }
    obstacle_group.push_back(self);
    p.obstacles.push_back(std::move(k));
  }

  // Nets: the groups that hold a connection, in order of their first connection.
  std::map<int, int> net_of_root;
  for (std::size_t i = 0; i < p.connections.size(); ++i) {
    auto& c = p.connections[i];
    const int root = g.find(g.id(c.name));
    const auto [it, fresh] = net_of_root.emplace(root, static_cast<int>(p.nets.size()));
    if (fresh) p.nets.push_back(SrjNet{c.name, {}, {}});
    c.net = it->second;
    p.nets[static_cast<std::size_t>(c.net)].connections.push_back(static_cast<int>(i));
  }
  for (std::size_t i = 0; i < p.obstacles.size(); ++i) {
    if (obstacle_group[i] < 0) continue;
    const auto it = net_of_root.find(g.find(obstacle_group[i]));
    if (it == net_of_root.end()) continue;
    p.obstacles[i].net = it->second;
    p.nets[static_cast<std::size_t>(it->second)].obstacles.push_back(static_cast<int>(i));
  }

  if (d.contains("tracemakerTrackLayers") && d["tracemakerTrackLayers"].is_array())
    for (const auto& l : d["tracemakerTrackLayers"])
      if (l.is_string()) {
        const int z = srj_layer_index(l.get<std::string>(), p.layer_count);
        if (z >= 0) p.track_layers.push_back(z);
      }
  auto warn = [&](const std::string& w) { p.warnings.push_back(w); };
  auto present = [&](const char* key) { return d.contains(key) && d[key].is_array() && !d[key].empty(); };
  if (present("traces")) warn("the problem's preloaded 'traces' are not imported: the board starts without them");
  if (present("differentialPairs")) warn("'differentialPairs' are not imported: the pairs are routed as single nets");
  if (present("buses")) {
    std::map<std::string, int> conn_of;
    for (std::size_t i = 0; i < p.connections.size(); ++i) conn_of[p.connections[i].name] = static_cast<int>(i);
    std::set<std::pair<int, int>> seen;
    int plane_buses = 0;
    int exit_layers = 0;
    for (const auto& bu : d["buses"]) {
      // A bus may name the layer each connection leaves on (connectionExitTargets, as the fan-out benchmark's problems
      // do): the breakout point at the boundary is then on that layer, not on the layer the point was written with.
      if (bu.contains("connectionExitTargets") && bu["connectionExitTargets"].is_object())
        for (const auto& [name, tgt] : bu["connectionExitTargets"].items()) {
          const auto it = conn_of.find(name);
          if (it == conn_of.end() || !tgt.is_object()) continue;
          auto& pts = p.connections[static_cast<std::size_t>(it->second)].points;
          const int z = srj_layer_index(opt_string(tgt, "layer"), p.layer_count);
          if (pts.size() < 2 || z < 0 || pts.back().point_id.rfind("pcb_breakout_point", 0) != 0) continue;
          if (std::getenv("TM_SRJ_EXIT_LAYERS")) pts.back().layers = {z};  // experiment: exit layers can stack terminals of different buses on one layer
          ++exit_layers;
        }
      if (!bu.contains("termination") || !bu["termination"].is_object() || opt_string(bu["termination"], "type") != "plane") continue;
      const int layer = srj_layer_index(opt_string(bu["termination"], "layer"), p.layer_count);
      if (layer < 0 || !bu.contains("connectionNames")) continue;
      ++plane_buses;
      for (const auto& cn : bu["connectionNames"]) {
        const auto it = conn_of.find(cn.is_string() ? cn.get<std::string>() : std::string());
        if (it == conn_of.end()) continue;
        const int net = p.connections[static_cast<std::size_t>(it->second)].net;
        if (seen.insert({net, layer}).second) p.planes.push_back(SrjPlane{net, layer});
      }
    }
    if (exit_layers) warn(std::to_string(exit_layers) + " breakout points moved to the layer their bus names in connectionExitTargets");
    warn("'buses' are not imported: their nets are routed one by one" +
         std::string(plane_buses ? "; " + std::to_string(plane_buses) + " plane terminations become zones" : ""));
  }
  if (present("jumpers") || flag(d, "allowJumpers")) warn("jumpers are not supported: connections are routed in copper only");
  if (off_board) warn(std::to_string(off_board) + " off-board connections are imported as ordinary connections");
  if (assignable) warn(std::to_string(assignable) + " obstacles with 'netIsAssignable' are imported with the net they have");
  if (terminal_vias) warn(std::to_string(terminal_vias) + " 'terminalVia' hints are ignored");
  return p;
}

SrjProblem read_srj_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream s;
  s << f.rdbuf();
  return read_srj(s.str());
}

}  // namespace tmk::io
