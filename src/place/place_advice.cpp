// SPDX-License-Identifier: GPL-3.0-or-later
#include "place/place_advice.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace tmk::place {

namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string& source, const std::string& where, const std::string& what) {
  throw std::runtime_error(source + ": " + where + ": " + what);
}

void only_keys(const json& j, const std::set<std::string>& allowed, const std::string& source, const std::string& where) {
  if (!j.is_object()) fail(source, where, "must be an object");
  for (const auto& [k, v] : j.items())
    if (!allowed.count(k)) fail(source, where, "unknown key \"" + k + "\"");
}

double number(const json& j, const std::string& source, const std::string& where) {
  if (!j.is_number()) fail(source, where, "must be a number");
  const double v = j.get<double>();
  if (!std::isfinite(v) || std::fabs(v) > 1e5) fail(source, where, "out of range");
  return v;
}

AdvisedMove move_of(const json& j, const Problem& p, const std::map<std::string, int>& by_ref, const std::string& source, const std::string& where) {
  only_keys(j, {"ref", "at", "rotate"}, source, where);
  if (!j.contains("ref") || !j["ref"].is_string()) fail(source, where + ".ref", "a footprint reference is required");
  const std::string ref = j["ref"].get<std::string>();
  const auto it = by_ref.find(ref);
  if (it == by_ref.end()) fail(source, where + ".ref", "no footprint \"" + ref + "\" on the board");
  if (it->second < 0) fail(source, where + ".ref", "the reference \"" + ref + "\" names more than one footprint");
  const Part& pt = p.parts[z(it->second)];
  if (!pt.movable)  // rule 6: a locked or fixed part never moves, whoever proposes it
    fail(source, where + ".ref", ref + " may not move (" + (pt.fixed_reason.empty() ? std::string("fixed") : pt.fixed_reason) + ")");
  if (!j.contains("at") || !j["at"].is_array() || j["at"].size() != 2) fail(source, where + ".at", "must be [x, y] in millimetres");
  AdvisedMove m;
  m.part = it->second;
  m.pos = Point{static_cast<Coord>(std::llround(number(j["at"][0], source, where + ".at[0]") * 1e6)),
                static_cast<Coord>(std::llround(number(j["at"][1], source, where + ".at[1]") * 1e6))};
  double deg = pt.angle0;
  if (j.contains("rotate")) {
    deg = number(j["rotate"], source, where + ".rotate");
    const double k = (deg - pt.angle0) / 90.0;
    if (std::fabs(k - std::round(k)) > 1e-6)
      fail(source, where + ".rotate", ref + " can take its own orientation (" + std::to_string(pt.angle0) + ") plus multiples of 90 degrees only");
    m.rot = static_cast<std::uint8_t>(((std::llround(k) % 4) + 4) % 4);
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s to (%.2f, %.2f) at %g deg", ref.c_str(), static_cast<double>(m.pos.x) / 1e6, static_cast<double>(m.pos.y) / 1e6,
                std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0));
  m.what = buf;
  return m;
}

}  // namespace

PlaceAdvice parse_place_advice(std::string_view text, const Problem& p, const std::string& source) {
  json j;
  try {
    j = json::parse(text);
  } catch (const json::parse_error& e) {
    throw std::runtime_error(source + ": not valid JSON: " + e.what());
  }
  only_keys(j, {"version", "note", "seeds", "eco"}, source, "top level");
  if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1) fail(source, "version", "must be 1");
  std::map<std::string, int> by_ref;  // -1: the reference is not unique
  for (std::size_t i = 0; i < p.parts.size(); ++i) {
    const auto [it, fresh] = by_ref.emplace(p.parts[i].ref, static_cast<int>(i));
    if (!fresh) it->second = -1;
  }
  PlaceAdvice a;
  a.source = source;
  if (j.contains("note")) {
    if (!j["note"].is_string()) fail(source, "note", "must be a string");
    a.note = j["note"].get<std::string>().substr(0, 2000);
  }
  if (j.contains("seeds")) {
    if (!j["seeds"].is_array()) fail(source, "seeds", "must be an array");
    if (j["seeds"].size() > kMaxAdviceSeeds) fail(source, "seeds", "at most " + std::to_string(kMaxAdviceSeeds) + " seeds");
    for (std::size_t s = 0; s < j["seeds"].size(); ++s) {
      const std::string w = "seeds[" + std::to_string(s) + "]";
      const auto& js = j["seeds"][s];
      only_keys(js, {"name", "moves"}, source, w);
      AdvisedSeed seed;
      seed.name = js.contains("name") && js["name"].is_string() ? js["name"].get<std::string>().substr(0, 80) : w;
      if (js.contains("name") && !js["name"].is_string()) fail(source, w + ".name", "must be a string");
      if (!js.contains("moves") || !js["moves"].is_array() || js["moves"].empty()) fail(source, w + ".moves", "must be a non-empty array");
      if (js["moves"].size() > kMaxSeedMoves) fail(source, w + ".moves", "at most " + std::to_string(kMaxSeedMoves) + " moves");
      std::set<int> seen;
      for (std::size_t k = 0; k < js["moves"].size(); ++k) {
        const std::string wm = w + ".moves[" + std::to_string(k) + "]";
        auto m = move_of(js["moves"][k], p, by_ref, source, wm);
        if (!seen.insert(m.part).second) fail(source, wm + ".ref", "the part is already moved by this seed");
        seed.moves.push_back(std::move(m));
      }
      a.seeds.push_back(std::move(seed));
    }
  }
  if (j.contains("eco")) {
    if (!j["eco"].is_array()) fail(source, "eco", "must be an array");
    if (j["eco"].size() > kMaxEcoMoves) fail(source, "eco", "at most " + std::to_string(kMaxEcoMoves) + " moves");
    for (std::size_t k = 0; k < j["eco"].size(); ++k) a.eco.push_back(move_of(j["eco"][k], p, by_ref, source, "eco[" + std::to_string(k) + "]"));
  }
  return a;
}

PlaceAdvice read_place_advice(const std::string& path, const Problem& p) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error(path + ": cannot read the placement advice file");
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_place_advice(ss.str(), p, path);
}

Placement apply_seed(const Placement& base, const AdvisedSeed& s) {
  Placement t = base;
  for (const auto& m : s.moves) {
    t.pos[z(m.part)] = m.pos;
    t.rot[z(m.part)] = m.rot;
  }
  return t;
}

}  // namespace tmk::place
