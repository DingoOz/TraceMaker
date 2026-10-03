#include "drc/rule_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace tmk::drc {

// ---------------------------------------------------------------------------------------------------------
// Condition expressions: a small recursive-descent parser for the subset of KiCad's rule language used in
// practice: A./B. properties, string/number literals, == != && || ! and parentheses, and a few functions.
// ---------------------------------------------------------------------------------------------------------
struct EvalCtx {
  const RuleEngine* eng;
  const CopperItem* a;
  const CopperItem* b;
  int layer;
  bool unknown = false;  // set when an unsupported property or function is evaluated
};

struct Value {
  enum class K { Undef, Bool, Str, Num } k = K::Undef;
  bool b = false;
  std::string s;
  double n = 0;
  bool truthy() const { return k == K::Bool ? b : k == K::Num ? n != 0 : k == K::Str ? !s.empty() : false; }
};

class Condition {
 public:
  struct Node {
    enum class Op { Or, And, Not, Eq, Ne, Lit, Prop, Call } op;
    std::vector<std::unique_ptr<Node>> kids;
    Value lit;
    char who = 'A';       // A or B
    std::string name;     // property or function name
    std::vector<std::string> args;
  };

  static std::unique_ptr<Condition> parse(const std::string& text, std::string& err) {
    auto c = std::make_unique<Condition>();
    c->src_ = text;
    c->pos_ = 0;
    try {
      c->root_ = c->parse_or();
      c->skip();
      if (c->pos_ != c->src_.size()) throw std::runtime_error("unexpected '" + c->src_.substr(c->pos_, 10) + "'");
    } catch (const std::exception& e) {
      err = e.what();
      return nullptr;
    }
    return c;
  }

  bool eval(EvalCtx& ctx) const { return eval(*root_, ctx).truthy(); }

 private:
  void skip() {
    while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
  }
  bool eat(std::string_view tok) {
    skip();
    if (src_.compare(pos_, tok.size(), tok) == 0) {
      pos_ += tok.size();
      return true;
    }
    return false;
  }
  std::unique_ptr<Node> make(Node::Op op) {
    auto n = std::make_unique<Node>();
    n->op = op;
    return n;
  }
  std::unique_ptr<Node> parse_or() {
    auto l = parse_and();
    while (eat("||")) {
      auto n = make(Node::Op::Or);
      n->kids.push_back(std::move(l));
      n->kids.push_back(parse_and());
      l = std::move(n);
    }
    return l;
  }
  std::unique_ptr<Node> parse_and() {
    auto l = parse_unary();
    while (eat("&&")) {
      auto n = make(Node::Op::And);
      n->kids.push_back(std::move(l));
      n->kids.push_back(parse_unary());
      l = std::move(n);
    }
    return l;
  }
  std::unique_ptr<Node> parse_unary() {
    skip();
    if (pos_ < src_.size() && src_[pos_] == '!' && (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '=')) {
      ++pos_;
      auto n = make(Node::Op::Not);
      n->kids.push_back(parse_unary());
      return n;
    }
    auto l = parse_term();
    if (eat("==")) {
      auto n = make(Node::Op::Eq);
      n->kids.push_back(std::move(l));
      n->kids.push_back(parse_term());
      return n;
    }
    if (eat("!=")) {
      auto n = make(Node::Op::Ne);
      n->kids.push_back(std::move(l));
      n->kids.push_back(parse_term());
      return n;
    }
    return l;
  }
  std::string read_string() {
    const char q = src_[pos_++];
    std::string s;
    while (pos_ < src_.size() && src_[pos_] != q) s += src_[pos_++];
    if (pos_ >= src_.size()) throw std::runtime_error("unterminated string");
    ++pos_;
    return s;
  }
  std::unique_ptr<Node> parse_term() {
    skip();
    if (pos_ >= src_.size()) throw std::runtime_error("unexpected end");
    const char c = src_[pos_];
    if (c == '(') {
      ++pos_;
      auto n = parse_or();
      if (!eat(")")) throw std::runtime_error("missing ')'");
      return n;
    }
    if (c == '\'' || c == '"') {
      auto n = make(Node::Op::Lit);
      n->lit.k = Value::K::Str;
      n->lit.s = read_string();
      return n;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
      std::size_t b = pos_;
      while (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.' || src_[pos_] == '-')) ++pos_;
      auto n = make(Node::Op::Lit);
      n->lit.k = Value::K::Num;
      n->lit.n = std::atof(src_.substr(b, pos_ - b).c_str());
      return n;
    }
    // Identifier: A.Prop, B.func(args), or a bare word (true/false).
    std::size_t b = pos_;
    while (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' || src_[pos_] == '.')) ++pos_;
    std::string id = src_.substr(b, pos_ - b);
    if (id.empty()) throw std::runtime_error(std::string("unexpected '") + c + "'");
    if (id == "true" || id == "false") {
      auto n = make(Node::Op::Lit);
      n->lit.k = Value::K::Bool;
      n->lit.b = id == "true";
      return n;
    }
    auto n = make(Node::Op::Prop);
    if (id.size() > 2 && (id[0] == 'A' || id[0] == 'B') && id[1] == '.') {
      n->who = id[0];
      n->name = id.substr(2);
    } else {
      n->name = id;
    }
    if (eat("(")) {
      n->op = Node::Op::Call;
      skip();
      while (!eat(")")) {
        skip();
        if (pos_ < src_.size() && (src_[pos_] == '\'' || src_[pos_] == '"')) n->args.push_back(read_string());
        else throw std::runtime_error("function arguments must be strings");
        eat(",");
      }
    }
    return n;
  }

  static bool str_eq(const Value& l, const Value& r) {
    if (l.k == Value::K::Num || r.k == Value::K::Num) {
      const double a = l.k == Value::K::Num ? l.n : std::atof(l.s.c_str());
      const double b = r.k == Value::K::Num ? r.n : std::atof(r.s.c_str());
      return std::fabs(a - b) < 1e-9;
    }
    if (l.k == Value::K::Bool || r.k == Value::K::Bool) return l.truthy() == r.truthy();
    // KiCad compares strings case-insensitively with wildcard support on either side.
    auto low = [](std::string s) {
      for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return s;
    };
    const std::string a = low(l.s), b = low(r.s);
    return model::wildcard_match(b, a) || model::wildcard_match(a, b);
  }

  Value prop(const Node& n, EvalCtx& ctx) const {
    const CopperItem* it = n.who == 'A' ? ctx.a : ctx.b;
    Value v;
    if (!it) return v;  // B absent (single-item constraint): undefined → false
    const auto& b = ctx.eng->b_;
    v.k = Value::K::Str;
    if (n.name == "NetClass") v.s = ctx.eng->netclass(*it).name;
    else if (n.name == "NetName") v.s = b.nets[static_cast<std::size_t>(it->net)].name;
    else if (n.name == "Type") v.s = it->kind == ItemKind::Arc ? "Track" : kind_name(it->kind);
    else if (n.name == "Layer") v.s = ctx.layer >= 0 ? b.copper_name(ctx.layer) : "";
    else if (n.name == "Reference" || n.name == "Parent.Reference")
      v.s = it->footprint >= 0 ? b.footprints[static_cast<std::size_t>(it->footprint)].reference : "";
    else if (n.name == "Pad_Type" && it->kind == ItemKind::Pad) {
      static const char* names[] = {"SMD", "Through-hole", "NPTH, mechanical", "Edge connector"};
      v.s = names[static_cast<int>(b.pads[static_cast<std::size_t>(it->index)].type)];
    } else if (n.name == "Width") {
      v.k = Value::K::Num;
      v.n = static_cast<double>(it->width) / 1e6;
    } else {
      ctx.unknown = true;
      v.k = Value::K::Undef;
    }
    return v;
  }

  Value call(const Node& n, EvalCtx& ctx) const {
    const CopperItem* it = n.who == 'A' ? ctx.a : ctx.b;
    Value v;
    v.k = Value::K::Bool;
    if (!it) return v;
    const auto& b = ctx.eng->b_;
    if (n.name == "isPlated") {
      v.b = it->kind == ItemKind::Via || (it->kind == ItemKind::Pad && b.pads[static_cast<std::size_t>(it->index)].type == model::PadType::ThruHole);
    } else if (n.name == "existsOnLayer" && !n.args.empty()) {
      const int l = b.copper_index(n.args[0]);
      v.b = l >= 0 && (it->layers & model::layer_bit(l));
    } else if ((n.name == "insideArea" || n.name == "intersectsArea" || n.name == "enclosedByArea") && !n.args.empty()) {
      for (const auto& z : b.zones) {
        if (!model::wildcard_match(n.args[0], z.name) || z.outline.empty()) continue;
        const auto& poly = z.outline.front();
        const bool all_in = geom::point_in_polygon({it->box.x0, it->box.y0}, poly) && geom::point_in_polygon({it->box.x1, it->box.y1}, poly) &&
                            geom::point_in_polygon({it->box.x0, it->box.y1}, poly) && geom::point_in_polygon({it->box.x1, it->box.y0}, poly);
        bool any = geom::point_in_polygon(it->pos, poly);
        if (!any) {
          const geom::Shape area = geom::Shape::polygon(poly, 0);
          for (const auto& s : it->shapes)
            if (geom::closer_than(s, area, 1)) { any = true; break; }
        }
        if (n.name == "intersectsArea" ? any : all_in) { v.b = true; break; }
      }
    } else if (n.name == "inDiffPair" && !n.args.empty()) {
      // KiCad: true when the item's net is one half of a differential pair whose base name (without the final
      // P/N or +/-) matches the pattern.
      const auto net = static_cast<std::size_t>(it->net);
      if (it->net != 0 && net < ctx.eng->dp_partner_.size() && ctx.eng->dp_partner_[net] != 0) {
        const std::string& name = b.nets[net].name;
        v.b = model::wildcard_match(n.args[0], name.substr(0, name.size() - 1)) || model::wildcard_match(n.args[0], name);
      }
    } else if (n.name == "memberOfFootprint" && !n.args.empty()) {
      v.b = it->footprint >= 0 && model::wildcard_match(n.args[0], b.footprints[static_cast<std::size_t>(it->footprint)].reference);
    } else {
      ctx.unknown = true;
      v.k = Value::K::Undef;
    }
    return v;
  }

  Value eval(const Node& n, EvalCtx& ctx) const {
    Value v;
    v.k = Value::K::Bool;
    switch (n.op) {
      case Node::Op::Or: v.b = eval(*n.kids[0], ctx).truthy() || eval(*n.kids[1], ctx).truthy(); return v;
      case Node::Op::And: v.b = eval(*n.kids[0], ctx).truthy() && eval(*n.kids[1], ctx).truthy(); return v;
      case Node::Op::Not: v.b = !eval(*n.kids[0], ctx).truthy(); return v;
      case Node::Op::Eq: v.b = str_eq(eval(*n.kids[0], ctx), eval(*n.kids[1], ctx)); return v;
      case Node::Op::Ne: v.b = !str_eq(eval(*n.kids[0], ctx), eval(*n.kids[1], ctx)); return v;
      case Node::Op::Lit: return n.lit;
      case Node::Op::Prop: return prop(n, ctx);
      case Node::Op::Call: return call(n, ctx);
    }
    return v;
  }

  std::string src_;
  std::size_t pos_ = 0;
  std::unique_ptr<Node> root_;
};

// ---------------------------------------------------------------------------------------------------------

RuleEngine::RuleEngine(const model::Board& b, const model::DesignRules& r, const CopperModel& cm) : b_(b), r_(r), cm_(cm) {
  for (const auto& rule : r_.custom) {
    Compiled c{&rule, nullptr, true};
    if (!rule.condition.empty()) {
      std::string err;
      c.cond = Condition::parse(rule.condition, err);
      if (!c.cond) {
        c.valid = false;
        warnings_.push_back("rule '" + rule.name + "': cannot parse condition (" + err + "); rule ignored");
      }
    }
    rules_.push_back(std::move(c));
    for (const auto& k : rule.constraints)
      if (k.type == "clearance" && k.min) max_clearance_ = std::max(max_clearance_, *k.min);
  }
  for (const auto& c : r_.classes) max_clearance_ = std::max(max_clearance_, c.clearance);
  // Per-net caches: net class and diff-pair partner (string matching is far too slow for inner loops).
  net_class_.resize(b_.nets.size());
  dp_partner_.assign(b_.nets.size(), 0);
  std::map<std::string, model::NetId> by_name;
  for (const auto& n : b_.nets) by_name[n.name] = n.id;
  for (const auto& n : b_.nets) {
    net_class_[static_cast<std::size_t>(n.id)] = &r_.class_for(n.name);
    if (n.name.size() < 2) continue;
    std::string other = n.name;
    char& last = other.back();
    if (last == 'P') last = 'N';
    else if (last == 'N') last = 'P';
    else if (last == '+') last = '-';
    else if (last == '-') last = '+';
    else continue;
    if (auto it = by_name.find(other); it != by_name.end()) dp_partner_[static_cast<std::size_t>(n.id)] = it->second;
  }
  for (const auto& rule : r_.custom)
    for (const auto& k : rule.constraints)
      if (k.type == "clearance") any_custom_clearance_ = true;
  for (const auto& p : b_.pads) max_clearance_ = std::max(max_clearance_, p.clearance);
  max_clearance_ = std::max({max_clearance_, r_.minimums.clearance, r_.minimums.copper_edge_clearance, r_.minimums.hole_clearance,
                             r_.minimums.hole_to_hole});
}

RuleEngine::~RuleEngine() = default;

const model::NetClass& RuleEngine::netclass(const CopperItem& it) const {
  const auto i = static_cast<std::size_t>(it.net);
  if (i < net_class_.size()) return *net_class_[i];
  return r_.class_for(b_.nets[i].name);
}

bool RuleEngine::layer_matches(const std::string& sel, int layer) const {
  if (sel.empty() || layer < 0) return true;
  const int last = b_.copper_count() - 1;
  if (sel == "outer") return layer == 0 || layer == last;
  if (sel == "inner") return layer > 0 && layer < last;
  return b_.copper_index(sel) == layer;
}

std::optional<Coord> RuleEngine::custom_min(const char* type, const CopperItem* a, const CopperItem* b, int layer) const {
  std::optional<Coord> out;
  for (const auto& c : rules_) {
    if (!c.valid || !layer_matches(c.rule->layer, layer)) continue;
    const model::Constraint* k = nullptr;
    for (const auto& x : c.rule->constraints)
      if (x.type == type && x.min) k = &x;
    if (!k) continue;
    bool match = true;
    if (c.cond) {
      EvalCtx ctx{this, a, b, layer};
      match = c.cond->eval(ctx);
      if (!match && b) {
        EvalCtx ctx2{this, b, a, layer};
        match = c.cond->eval(ctx2);
      }
    }
    if (match) out = *k->min;  // later rules take precedence
  }
  return out;
}

std::pair<std::optional<Coord>, std::optional<Coord>> RuleEngine::length_constraint(model::NetId net) const {
  std::pair<std::optional<Coord>, std::optional<Coord>> out;
  CopperItem probe;
  probe.kind = ItemKind::Track;
  probe.net = net;
  probe.layers = ~model::LayerMask{0};
  for (const auto& c : rules_) {
    if (!c.valid) continue;
    const model::Constraint* k = nullptr;
    for (const auto& x : c.rule->constraints)
      if (x.type == "length") k = &x;
    if (!k) continue;
    if (c.cond) {
      EvalCtx ctx{this, &probe, nullptr, -1};
      if (!c.cond->eval(ctx)) continue;
    }
    out = {k->min, k->max};  // later rules take precedence
  }
  return out;
}

std::optional<Coord> RuleEngine::skew_constraint(model::NetId net) const {
  std::optional<Coord> out;
  CopperItem probe;
  probe.kind = ItemKind::Track;
  probe.net = net;
  probe.layers = ~model::LayerMask{0};
  for (const auto& c : rules_) {
    if (!c.valid) continue;
    const model::Constraint* k = nullptr;
    for (const auto& x : c.rule->constraints)
      if (x.type == "skew" && x.max) k = &x;
    if (!k) continue;
    if (c.cond) {
      EvalCtx ctx{this, &probe, nullptr, -1};
      if (!c.cond->eval(ctx)) continue;
    }
    out = k->max;
  }
  return out;
}

bool RuleEngine::coupled_diff_pair(model::NetId a, model::NetId b) const {
  if (a == 0 || b == 0 || a == b) return false;
  if (static_cast<std::size_t>(a) < dp_partner_.size()) return dp_partner_[static_cast<std::size_t>(a)] == b;
  const std::string& x = b_.nets[static_cast<std::size_t>(a)].name;
  const std::string& y = b_.nets[static_cast<std::size_t>(b)].name;
  if (x.size() != y.size() || x.empty()) return false;
  const char cx = x.back(), cy = y.back();
  const bool pair = (cx == 'P' && cy == 'N') || (cx == 'N' && cy == 'P') || (cx == '+' && cy == '-') || (cx == '-' && cy == '+');
  return pair && x.compare(0, x.size() - 1, y, 0, y.size() - 1) == 0;
}

Coord RuleEngine::clearance(const CopperItem& a, const CopperItem& b, int layer) const {
  // Local overrides (pad, then footprint) win over net classes: KiCad uses the larger of the two items'
  // overrides when either has one (verified on tinytapeout-demo: U6 footprint 0.18 mm vs GND class 0.23 mm).
  auto local = [&](const CopperItem& it) -> Coord {
    if (it.kind != ItemKind::Pad) return -1;
    const auto& pad = b_.pads[static_cast<std::size_t>(it.index)];
    if (pad.clearance >= 0) return pad.clearance;
    return b_.footprints[static_cast<std::size_t>(pad.footprint)].clearance;
  };
  const Coord la = local(a), lb = local(b);
  Coord req = (la >= 0 || lb >= 0) ? std::max(la, lb) : std::max(netclass(a).clearance, netclass(b).clearance);
  // KiCad's implicit diff-pair rule: between the two halves of a coupled pair, the class's diff-pair gap
  // replaces the clearance (verified on CM5_MINIMA_3: PCIe P/N at 0.177 mm under a 0.2 mm class clearance).
  // Observed: the smaller of clearance and diff-pair gap applies (0.154 gap passes under a 0.2 class; 0.2 mm
  // pads pass under a 0.25 gap).
  if (la < 0 && lb < 0 && coupled_diff_pair(a.net, b.net)) req = std::min(req, netclass(a).diff_pair_gap);
  if (any_custom_clearance_)
    if (auto c = custom_min("clearance", &a, &b, layer)) req = *c;
  // The board minimum is always a floor (multichannel_mixer: 0.3 mm minimum over a 0.2 mm class).
  return std::max(req, r_.minimums.clearance);
}

Coord RuleEngine::hole_clearance(const CopperItem* owner, const CopperItem& other, int layer) const {
  Coord req = r_.minimums.hole_clearance;
  if (auto c = custom_min("hole_clearance", owner, &other, layer)) req = std::max(req, *c);
  return req;
}

Coord RuleEngine::hole_to_hole(const CopperItem* a, const CopperItem* b) const {
  Coord req = r_.minimums.hole_to_hole;
  if (auto c = custom_min("hole_to_hole", a, b, -1)) req = std::max(req, *c);
  return req;
}

Coord RuleEngine::edge_clearance(const CopperItem& a, int layer) const {
  Coord req = r_.minimums.copper_edge_clearance;
  if (auto c = custom_min("edge_clearance", &a, nullptr, layer)) req = *c;
  return req;
}

std::pair<Coord, Coord> RuleEngine::track_width(const CopperItem& t, int layer) const {
  Coord mn = r_.minimums.track_width, mx = 0;
  if (auto c = custom_min("track_width", &t, nullptr, layer)) mn = *c;
  return {mn, mx};
}

Coord RuleEngine::via_diameter_min(const CopperItem& v) const {
  Coord req = r_.minimums.via_diameter;
  if (auto c = custom_min("via_diameter", &v, nullptr, -1)) req = *c;
  return req;
}

Coord RuleEngine::annular_width_min(const CopperItem& v) const {
  Coord req = r_.minimums.via_annular_width;
  if (auto c = custom_min("annular_width", &v, nullptr, -1)) req = *c;
  return req;
}

Coord RuleEngine::hole_size_min(const CopperItem* owner) const {
  Coord req = r_.minimums.through_hole_diameter;
  if (auto c = custom_min("hole_size", owner, nullptr, -1)) req = *c;
  return req;
}

}  // namespace tmk::drc
