// SPDX-License-Identifier: GPL-3.0-or-later
// Critical nets from component-rule roles (issue #24, doc 05 §44): see critical.hpp.
#include "crules/critical.hpp"

#include <algorithm>
#include <map>

namespace tmk::crules {

// Each line is a role of the catalogue (docs/15-component-rules.md) that carries a signal-integrity rule: impedance, differential pair, length match
// or via limit (USB2-01/02/04, USBC-01/02/04, ETH-01/02, HDMI-01/02, LVDS-01/03, MIPI-01/02, PCIE-01/02, SATA-01/02, DDR3-01, DDR4-01, PBUS-03,
// QSPI-02, SDIO-01, XTAL-03, RF-01/02). Tier 1 are the roles of clocks, crystals and RF lines.
const std::vector<CriticalRole>& critical_roles() {
  static const std::vector<CriticalRole> t = {
      {"crystal", "xin", 1},       {"crystal", "xout", 1},      {"oscillator", "out", 1},     {"rf_line", "line", 1},      {"pcie", "refclk", 1},
      {"mipi_dphy", "clk", 1},     {"ddr3", "ck", 1},           {"ddr4_lpddr4", "ck", 1},     {"sdr_sdram", "clk", 1},     {"qspi_flash", "clk", 1},
      {"sd_sdio", "clk", 1},       {"usb2", "dp", 2},           {"usb2", "dm", 2},            {"usb3_typec", "sstx", 2},   {"usb3_typec", "ssrx", 2},
      {"ethernet", "mdi_pairs", 2}, {"ethernet", "line_pairs", 2}, {"hdmi_dp", "tmds", 2},    {"lvds", "pairs", 2},        {"mipi_dphy", "data", 2},
      {"pcie", "tx", 2},           {"pcie", "rx", 2},           {"sata", "pairs", 2},         {"ddr3", "byte_lane", 3},    {"ddr3", "addr_cmd", 3},
      {"ddr4_lpddr4", "byte_lane", 3}, {"ddr4_lpddr4", "addr_cmd", 3}, {"sdr_sdram", "data", 3}, {"sdr_sdram", "addr", 3}, {"qspi_flash", "data", 3},
      {"sd_sdio", "bus", 3},       {"ethernet", "mac_bus", 3},
  };
  return t;
}

std::vector<CriticalNet> critical_nets(const model::Board& b, const Catalogue& cat, const Detection& det,
                                       const std::vector<std::pair<model::NetId, model::NetId>>& diff_pairs) {
  std::map<model::NetId, CriticalNet> found;  // ordered by net id
  auto add = [&](model::NetId n, int tier, const std::string& why) {
    if (n <= 0 || static_cast<std::size_t>(n) >= b.nets.size()) return;
    auto it = found.find(n);
    if (it == found.end()) found.emplace(n, CriticalNet{n, tier, why});
    else if (tier < it->second.tier) it->second = CriticalNet{n, tier, why};
  };
  for (const auto& in : det.instances) {
    if (in.category < 0 || static_cast<std::size_t>(in.category) >= cat.categories.size()) continue;
    const Category& c = cat.categories[static_cast<std::size_t>(in.category)];
    for (const auto& cr : critical_roles()) {
      if (c.id != cr.category) continue;
      if (const Role* r = in.role(cr.role))
        for (const model::NetId n : r->nets) add(n, cr.tier, c.name + " (" + c.id + " " + cr.role + ")");
    }
  }
  for (const auto& [x, y] : diff_pairs) {
    add(x, 2, "differential pair");
    add(y, 2, "differential pair");
  }
  std::vector<CriticalNet> out;
  for (const auto& [n, c] : found) out.push_back(c);
  std::stable_sort(out.begin(), out.end(), [](const CriticalNet& a, const CriticalNet& c) { return a.tier < c.tier; });
  return out;
}

}  // namespace tmk::crules
