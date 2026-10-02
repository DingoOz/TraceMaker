#include "server/messages.hpp"

#include <cmath>
#include <cstdlib>
#include <map>

#include <nlohmann/json.hpp>

#include "drc/copper.hpp"
#include "geom/shape.hpp"

namespace tmk::server {
namespace {

using J = nlohmann::ordered_json;
using model::Point;

J pt(Point p) { return J::array({p.x, p.y}); }

J pts_json(std::span<const Point> pts) {
  J a = J::array();
  for (const auto& p : pts) a.push_back(pt(p));
  return a;
}

J box_json(const geom::Box& b) { return J::array({b.x0, b.y0, b.x1, b.y1}); }

J layers_json(model::LayerMask m) {
  J a = J::array();
  for (int i = 0; i < 64; ++i)
    if (m & model::layer_bit(i)) a.push_back(i);
  return a;
}

std::string dump(const J& j) { return j.dump(-1, ' ', false, J::error_handler_t::replace); }

J track_json(ObjectId id, Point a, Point b, Coord w, int layer, model::NetId net) {
  J t;
  t["id"] = id;
  t["a"] = pt(a);
  t["b"] = pt(b);
  t["w"] = w;
  t["layer"] = layer;
  t["net"] = net;
  return t;
}

J via_json(ObjectId id, const model::Via& v) {
  J j;
  j["id"] = id;
  j["p"] = pt(v.pos);
  j["d"] = v.size;
  j["drill"] = v.drill;
  j["net"] = v.net;
  j["top"] = v.layer_top;
  j["bottom"] = v.layer_bottom;
  return j;
}

// Cubic Bézier through four control points, flattened to a fixed number of chords (outline display only).
std::vector<Point> bezier_points(const std::vector<Point>& c) {
  if (c.size() != 4) return c;
  std::vector<Point> out;
  constexpr int kSteps = 24;
  for (int i = 0; i <= kSteps; ++i) {
    const double t = i / static_cast<double>(kSteps), u = 1 - t;
    const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    auto f = [&](auto get) {
      return geom::kiround(w0 * static_cast<double>(get(c[0])) + w1 * static_cast<double>(get(c[1])) +
                           w2 * static_cast<double>(get(c[2])) + w3 * static_cast<double>(get(c[3])));
    };
    out.push_back({f([](Point p) { return p.x; }), f([](Point p) { return p.y; })});
  }
  return out;
}

// Edge.Cuts graphics as polylines (arcs and circles flattened).
std::vector<std::vector<Point>> edge_pieces(const model::Board& b) {
  std::vector<std::vector<Point>> pieces;
  for (const auto& g : b.graphics) {
    if (g.layer != "Edge.Cuts") continue;
    switch (g.kind) {
      case model::Graphic::Kind::Line: pieces.push_back({g.a, g.b}); break;
      case model::Graphic::Kind::Arc: pieces.push_back(geom::arc_points(g.a, g.c, g.b)); break;
      case model::Graphic::Kind::Circle: {
        const Coord r = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y)));
        pieces.push_back(geom::circle_points(g.a, r));
        break;
      }
      case model::Graphic::Kind::Rect:
      case model::Graphic::Kind::Poly:
        if (g.pts.size() >= 2) {
          auto p = g.pts;
          p.push_back(p.front());
          pieces.push_back(std::move(p));
        }
        break;
      case model::Graphic::Kind::Curve:
        if (g.pts.size() >= 2) pieces.push_back(bezier_points(g.pts));
        break;
    }
  }
  return pieces;
}

bool near(Point a, Point b) {
  constexpr Coord kTol = 2'000;  // 2 µm: KiCad outlines are joined exactly, allow rounding of flattened arcs
  return std::llabs(a.x - b.x) <= kTol && std::llabs(a.y - b.y) <= kTol;
}

// Joins outline pieces end to end into the longest chains possible (closed loops for a valid outline), so the
// viewer can fill the board substrate. Greedy and quadratic: outlines have at most a few thousand pieces.
std::vector<std::vector<Point>> chain_pieces(std::vector<std::vector<Point>> pieces) {
  std::vector<std::vector<Point>> loops;
  std::vector<bool> used(pieces.size(), false);
  for (std::size_t s = 0; s < pieces.size(); ++s) {
    if (used[s] || pieces[s].empty()) continue;
    used[s] = true;
    std::vector<Point> chain = pieces[s];
    bool grew = true;
    while (grew && !near(chain.front(), chain.back())) {
      grew = false;
      for (std::size_t k = 0; k < pieces.size(); ++k) {
        if (used[k] || pieces[k].empty()) continue;
        auto& p = pieces[k];
        if (near(chain.back(), p.front())) {
          chain.insert(chain.end(), p.begin() + 1, p.end());
        } else if (near(chain.back(), p.back())) {
          chain.insert(chain.end(), p.rbegin() + 1, p.rend());
        } else {
          continue;
        }
        used[k] = true;
        grew = true;
        break;
      }
    }
    loops.push_back(std::move(chain));
  }
  return loops;
}

void add_shape_pads(J& pads, int id, const model::Pad& pad, const geom::Shape& s) {
  auto emit = [&](const char* kind, J pts) {
    J p;
    p["id"] = id;
    p["net"] = pad.net;
    p["layers"] = layers_json(pad.copper);
    p["shape"] = kind;
    p["pts"] = std::move(pts);
    p["r"] = s.r;
    p["fp"] = pad.footprint;
    p["num"] = pad.number;
    if (pad.drill_x > 0) {
      // Hole as a rounded core: a point (round drill) or a segment along the long axis (oval drill).
      const Coord dx = pad.drill_x, dy = pad.drill_y > 0 ? pad.drill_y : pad.drill_x;
      const Coord half = std::abs(dx - dy) / 2;
      J h;
      if (half == 0) {
        h["pts"] = J::array({pt(pad.pos)});
      } else {
        const Point d = geom::rotate(dx > dy ? Point{half, 0} : Point{0, half}, pad.angle);
        h["pts"] = J::array({pt(pad.pos - d), pt(pad.pos + d)});
      }
      h["r"] = std::min(dx, dy) / 2;
      p["hole"] = std::move(h);
    }
    pads.push_back(std::move(p));
  };
  if (s.pts.size() == 1) {
    emit("circle", pts_json(s.pts));
  } else if (s.closed && s.pts.size() >= 3) {
    emit("polygon", pts_json(s.pts));
  } else {
    for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) emit("segment", J::array({pt(s.pts[i]), pt(s.pts[i + 1])}));
  }
}

}  // namespace

std::string board_snapshot_json(const model::Board& b, const std::string& name) {
  J j;
  j["type"] = "board";
  j["name"] = name;

  geom::Box bbox = b.edge_bbox();
  geom::Box content;

  J layers = J::array();
  for (int i = 0; i < b.copper_count(); ++i) {
    J l;
    l["name"] = b.copper_name(i);
    l["index"] = i;
    layers.push_back(std::move(l));
  }

  model::NetId max_net = 0;
  for (const auto& n : b.nets) max_net = std::max(max_net, n.id);
  std::vector<std::string> net_names(static_cast<std::size_t>(max_net) + 1);
  for (const auto& n : b.nets)
    if (n.id >= 0) net_names[static_cast<std::size_t>(n.id)] = n.name;
  J nets = J::array();
  for (auto& n : net_names) nets.push_back(std::move(n));

  J outline = J::array();
  for (const auto& loop : chain_pieces(edge_pieces(b))) outline.push_back(pts_json(loop));

  J pads = J::array();
  std::vector<geom::Box> fp_box(b.footprints.size());
  for (std::size_t i = 0; i < b.pads.size(); ++i) {
    const auto& pad = b.pads[i];
    for (const auto& s : drc::pad_shapes(pad)) {
      add_shape_pads(pads, static_cast<int>(i), pad, s);
      content.add(s.box);
      if (pad.footprint >= 0 && static_cast<std::size_t>(pad.footprint) < fp_box.size())
        fp_box[static_cast<std::size_t>(pad.footprint)].add(s.box);
    }
  }

  J tracks = J::array();
  for (std::size_t i = 0; i < b.tracks.size(); ++i) {
    const auto& t = b.tracks[i];
    tracks.push_back(track_json(static_cast<ObjectId>(i), t.a, t.b, t.width, t.layer, t.net));
    content.add(t.a);
    content.add(t.b);
  }
  ObjectId arc_id = -1;
  for (const auto& a : b.arcs) {
    const auto p = geom::arc_points(a.a, a.mid, a.b);
    for (std::size_t k = 0; k + 1 < p.size(); ++k) tracks.push_back(track_json(arc_id--, p[k], p[k + 1], a.width, a.layer, a.net));
  }

  J vias = J::array();
  for (std::size_t i = 0; i < b.vias.size(); ++i) {
    vias.push_back(via_json(static_cast<ObjectId>(i), b.vias[i]));
    content.add(b.vias[i].pos);
  }

  J zones = J::array();
  for (const auto& z : b.zones) {
    if (z.rule_area) continue;
    std::map<int, J> by_layer;
    for (const auto& [layer, poly] : z.fills) {
      if (layer < 0 || poly.size() < 3) continue;
      auto [it, fresh] = by_layer.try_emplace(layer, J::array());
      it->second.push_back(pts_json(poly));
    }
    for (auto& [layer, polys] : by_layer) {
      J zj;
      zj["layer"] = layer;
      zj["net"] = z.net;
      zj["polys"] = std::move(polys);
      zones.push_back(std::move(zj));
    }
  }

  J fps = J::array();
  for (std::size_t i = 0; i < b.footprints.size(); ++i) {
    const auto& f = b.footprints[i];
    J fj;
    fj["ref"] = f.reference;
    fj["x"] = f.pos.x;
    fj["y"] = f.pos.y;
    fj["angle"] = f.angle;
    fj["back"] = f.back;
    const geom::Box& fb = fp_box[i].empty() ? geom::Box{f.pos.x, f.pos.y, f.pos.x, f.pos.y} : fp_box[i];
    fj["bbox"] = box_json(fb);
    fj["value"] = f.value;
    fps.push_back(std::move(fj));
  }

  if (bbox.empty()) bbox = content;
  if (bbox.empty()) bbox = geom::Box{0, 0, 100 * kNmPerMm, 100 * kNmPerMm};

  j["bbox"] = box_json(bbox);
  j["layers"] = std::move(layers);
  j["nets"] = std::move(nets);
  j["outline"] = std::move(outline);
  j["pads"] = std::move(pads);
  j["tracks"] = std::move(tracks);
  j["vias"] = std::move(vias);
  j["zones"] = std::move(zones);
  j["footprints"] = std::move(fps);
  return dump(j);
}

std::string track_add(ObjectId id, const model::Track& t) { return track_add(id, t.a, t.b, t.width, t.layer, t.net); }

std::string track_add(ObjectId id, Point a, Point b, Coord width, int layer, model::NetId net) {
  J j;
  j["type"] = "track_add";
  j["track"] = track_json(id, a, b, width, layer, net);
  return dump(j);
}

std::string track_remove(ObjectId id) {
  J j;
  j["type"] = "track_remove";
  j["id"] = id;
  return dump(j);
}

std::string via_add(ObjectId id, const model::Via& v) {
  J j;
  j["type"] = "via_add";
  j["via"] = via_json(id, v);
  return dump(j);
}

std::string via_remove(ObjectId id) {
  J j;
  j["type"] = "via_remove";
  j["id"] = id;
  return dump(j);
}

std::string footprint_move(std::string_view ref, Point pos, double angle) {
  J j;
  j["type"] = "footprint_move";
  j["ref"] = ref;
  j["x"] = pos.x;
  j["y"] = pos.y;
  j["angle"] = angle;
  return dump(j);
}

std::string ratsnest(std::span<const RatsEdge> edges) {
  J e = J::array();
  for (const auto& r : edges) e.push_back(J::array({r.a.x, r.a.y, r.b.x, r.b.y, r.net}));
  J j;
  j["type"] = "ratsnest";
  j["edges"] = std::move(e);
  return dump(j);
}

std::string frontier(std::int64_t conn, int layer, std::span<const Point> pts) {
  J j;
  j["type"] = "frontier";
  j["conn"] = conn;
  j["layer"] = layer;
  j["pts"] = pts_json(pts);
  return dump(j);
}

std::string path_try(std::int64_t conn, std::span<const PathPoint> pts) {
  J a = J::array();
  for (const auto& p : pts) a.push_back(J::array({p.p.x, p.p.y, p.layer}));
  J j;
  j["type"] = "path_try";
  j["conn"] = conn;
  j["pts"] = std::move(a);
  return dump(j);
}

std::string failure(const Failure& f) {
  J j;
  j["type"] = "failure";
  j["conn"] = f.conn;
  j["net"] = f.net;
  j["rung"] = f.rung;
  j["cause"] = f.cause;
  j["a"] = pt(f.a);
  j["b"] = pt(f.b);
  j["blockers"] = f.blockers;
  if (!f.region.empty()) j["region"] = box_json(f.region);
  return dump(j);
}

std::string stats(const Stats& s) {
  J j;
  j["type"] = "stats";
  j["stage"] = s.stage;
  j["iteration"] = s.iteration;
  j["routed"] = s.routed;
  j["total"] = s.total;
  j["unrouted"] = s.unrouted;
  j["rips"] = s.rips;
  j["failures"] = s.failures;
  j["elapsed_s"] = s.elapsed_s;
  J extra = J::object();
  for (const auto& [k, v] : s.extra) extra[k] = v;
  j["extra"] = std::move(extra);
  return dump(j);
}

std::string stage(std::string_view name, std::string_view state, std::string_view detail) {
  J j;
  j["type"] = "stage";
  j["name"] = name;
  j["state"] = state;
  j["detail"] = detail;
  return dump(j);
}

std::string log(std::string_view level, std::string_view text) {
  J j;
  j["type"] = "log";
  j["level"] = level;
  j["text"] = text;
  return dump(j);
}

std::string heatmap(std::string_view name, Point origin, Coord cell, int w, int h, int layer, double max,
                    std::span<const std::uint8_t> data) {
  J j;
  j["type"] = "heatmap";
  j["name"] = name;
  j["x0"] = origin.x;
  j["y0"] = origin.y;
  j["cell"] = cell;
  j["w"] = w;
  j["h"] = h;
  j["layer"] = layer;
  j["max"] = max;
  j["data"] = std::vector<std::uint8_t>(data.begin(), data.end());
  return dump(j);
}

std::string message_type(std::string_view json) {
  std::size_t i = 0;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  constexpr std::string_view kPrefix = "{\"type\":\"";
  if (json.substr(i, kPrefix.size()) == kPrefix) {
    const std::size_t start = i + kPrefix.size();
    const std::size_t end = json.find('"', start);
    if (end != std::string_view::npos && end - start < 64) return std::string(json.substr(start, end - start));
  }
  const auto j = nlohmann::json::parse(json, nullptr, false);
  if (j.is_object()) {
    if (auto it = j.find("type"); it != j.end() && it->is_string()) return it->get<std::string>();
  }
  return {};
}

}  // namespace tmk::server
