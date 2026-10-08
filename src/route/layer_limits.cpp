// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/layer_limits.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace tmk::route {

namespace {

std::string copper_names(const model::Board& b) {
  std::string s;
  for (int i = 0; i < b.copper_count(); ++i) s += (i ? ", " : "") + b.copper_name(i);
  return s;
}

int copper_layer(const model::Board& b, const std::string& name, const char* option) {
  int idx = b.copper_index(name);
  for (int i = 0; idx < 0 && i < b.copper_count(); ++i) {
    const auto& l = b.layers[static_cast<std::size_t>(b.copper[static_cast<std::size_t>(i)])];
    if (!l.user_name.empty() && l.user_name == name) idx = i;
  }
  if (idx < 0) throw std::invalid_argument(std::string(option) + ": \"" + name + "\" is not a copper layer of this board (" + copper_names(b) + ")");
  return idx;
}

}  // namespace

LayerLimits layer_limits(const model::Board& b, const std::vector<std::string>& no_tracks_on, const std::vector<std::string>& layer_costs) {
  LayerLimits out;
  for (const auto& name : no_tracks_on) out.no_track_layers |= model::layer_bit(copper_layer(b, name, "--no-tracks-on"));
  const model::LayerMask all = b.copper_count() >= 64 ? ~model::LayerMask{0} : model::layer_bit(b.copper_count()) - 1;
  if (!no_tracks_on.empty() && (out.no_track_layers & all) == all) throw std::invalid_argument("--no-tracks-on: no copper layer is left for tracks");
  for (const auto& entry : layer_costs) {
    // The layer name may itself contain '=' only in theory; the factor is what follows the last one.
    const auto eq = entry.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) throw std::invalid_argument("--layer-cost: \"" + entry + "\" is not LAYER=FACTOR");
    const int idx = copper_layer(b, entry.substr(0, eq), "--layer-cost");
    double f = 0;
    const char* first = entry.data() + eq + 1;
    const char* last = entry.data() + entry.size();
    const auto [end, ec] = std::from_chars(first, last, f);
    // Below 1 a track would cost less than its length, which the search's distance bounds do not allow for.
    if (ec != std::errc() || end != last || !(f >= 1.0) || f > kMaxLayerCost)
      throw std::invalid_argument("--layer-cost: the factor in \"" + entry + "\" must be a number from 1 to " + std::to_string(static_cast<int>(kMaxLayerCost)));
    if (out.layer_cost_pm.empty()) out.layer_cost_pm.assign(static_cast<std::size_t>(b.copper_count()), 1000);
    out.layer_cost_pm[static_cast<std::size_t>(idx)] = static_cast<std::int32_t>(std::llround(f * 1000.0));
  }
  return out;
}

}  // namespace tmk::route
