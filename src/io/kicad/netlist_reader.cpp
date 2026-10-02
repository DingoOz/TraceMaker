#include "io/kicad/netlist_reader.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "sexpr/sexpr.hpp"

namespace tmk::io {

model::Netlist read_netlist_file(const std::string& path) {
  const auto d = sexpr::Document::load(path);
  if (d.head(d.root()) != "export") throw std::runtime_error(path + " is not a KiCad netlist");
  model::Netlist nl;
  auto field = [&](sexpr::NodeId n, const char* name) {
    const auto c = d.find(n, name);
    return c == sexpr::kNoNode ? std::string() : d.str_at(c, 1);
  };
  if (auto comps = d.find(d.root(), "components"); comps != sexpr::kNoNode)
    for (auto c : d.find_all(comps, "comp")) {
      model::NetlistComponent k{field(c, "ref"), field(c, "value"), field(c, "footprint"), false, false};
      for (auto p : d.find_all(c, "property")) {
        const auto pn = d.find(p, "name");
        const std::string name = pn == sexpr::kNoNode ? "" : d.str_at(pn, 1);
        if (name == "dnp") k.dnp = true;
        if (name == "exclude_from_board") k.exclude_from_board = true;
      }
      nl.components.push_back(std::move(k));
    }
  if (auto nets = d.find(d.root(), "nets"); nets != sexpr::kNoNode)
    for (auto n : d.find_all(nets, "net")) {
      model::NetlistNet net{field(n, "name"), field(n, "class"), {}};
      for (auto node : d.find_all(n, "node"))
        net.nodes.push_back({field(node, "ref"), field(node, "pin"), field(node, "pinfunction"), field(node, "pintype")});
      nl.nets.push_back(std::move(net));
    }
  return nl;
}

model::Netlist netlist_from_schematic(const std::string& sch_path, const std::string& tmp_dir) {
  namespace fs = std::filesystem;
  fs::create_directories(tmp_dir);
  const fs::path out = fs::path(tmp_dir) / (fs::path(sch_path).stem().string() + ".net");
  // Quote arguments for the shell; paths come from the user and may contain spaces.
  auto q = [](const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
  };
  const std::string cmd = "kicad-cli sch export netlist --format kicadsexpr -o " + q(out.string()) + " " + q(sch_path) + " >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0 || !fs::exists(out)) throw std::runtime_error("kicad-cli netlist export failed for " + sch_path);
  return read_netlist_file(out.string());
}

}  // namespace tmk::io
