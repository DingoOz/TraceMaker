// SPDX-License-Identifier: GPL-3.0-or-later
// Footprint check against datasheet land patterns (issue #5, doc 05 §43): see footprint_check.hpp.
#include "analysis/footprint_check.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>

#include "model/rules.hpp"

namespace tmk::analysis {

int FootprintResult::errors() const {
  return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const FootprintIssue& i) { return i.severity == Severity::Error; }));
}
int FootprintResult::warnings() const {
  return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const FootprintIssue& i) { return i.severity == Severity::Warning; }));
}

namespace {

using geom::Point;

struct Xy {
  double x = 0, y = 0;
};

// KiCad's rotation: positive angles turn counter-clockwise on screen in y-down coordinates (geom::rotate, in doubles).
Xy rot(double x, double y, double deg) {
  const double a = deg * std::numbers::pi / 180.0;
  const double c = std::cos(a), s = std::sin(a);
  return {x * c + y * s, -x * s + y * c};
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool wild_ci(const std::string& pattern, const std::string& text) { return model::wildcard_match(lower(pattern), lower(text)); }
bool any_wild(const std::vector<std::string>& patterns, const std::string& text) {
  return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& p) { return model::wildcard_match(p, text); });
}
// "Manufacturer Part Number", "MFR_PN" ... compared by their letters and digits only.
std::string key_norm(const std::string& s) {
  std::string out;
  for (const char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
double mm(double nm) { return nm / 1e6; }

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  std::string s = buf;
  while (s.size() > 1 && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}
std::string pad_name(const std::string& n) { return n.empty() ? "(unnumbered)" : n; }
model::PadType norm_type(model::PadType t) { return t == model::PadType::Connect ? model::PadType::Smd : t; }
const char* type_name(model::PadType t) {
  switch (norm_type(t)) {
    case model::PadType::ThruHole: return "plated through hole";
    case model::PadType::NpThruHole: return "non-plated hole";
    default: return "SMD pad";
  }
}

// The pads of a board footprint that are land pattern: copper pads and non-plated holes (paste and mask apertures are not), in the footprint's frame.
struct BoardPad {
  const model::Pad* p = nullptr;
  double x = 0, y = 0;
  double angle = 0;
};
std::vector<BoardPad> board_pads(const model::Board& b, const model::Footprint& fp) {
  std::vector<BoardPad> out;
  for (const int pi : fp.pads) {
    const model::Pad& p = b.pads[static_cast<std::size_t>(pi)];
    if (p.type != model::PadType::NpThruHole && p.copper == 0) continue;
    const Xy l = rot(static_cast<double>(p.pos.x - fp.pos.x), static_cast<double>(p.pos.y - fp.pos.y), -fp.angle);
    out.push_back({&p, l.x, l.y, p.angle - fp.angle});
  }
  return out;
}

// One way of laying the datasheet pattern over the footprint: turned by `rot` degrees, mirrored or not, moved by (tx, ty); and which board pad each
// datasheet pad went to.
struct Alignment {
  int rot = 0;
  bool mirror = false;
  double tx = 0, ty = 0;
  std::vector<int> to_board;  // per datasheet pad: the board pad it is paired with, -1 = none
  int matched = 0;
  double max_dev = 0, sum_dev = 0;
};

Xy transform(const RefPad& r, int rot_deg, bool mirror) { return rot(mirror ? -r.x : r.x, r.y, rot_deg); }

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

Alignment align(const std::vector<BoardPad>& bp, const std::vector<RefPad>& rp, int rot_deg, bool mirror) {
  Alignment a;
  a.rot = rot_deg;
  a.mirror = mirror;
  a.to_board.assign(rp.size(), -1);
  std::map<std::string, std::vector<int>> by_number;  // ordered: deterministic
  for (std::size_t j = 0; j < bp.size(); ++j) by_number[bp[j].p->number].push_back(static_cast<int>(j));
  std::vector<Xy> t(rp.size());
  for (std::size_t i = 0; i < rp.size(); ++i) t[i] = transform(rp[i], rot_deg, mirror);
  // The start: the footprint's pads and the pattern's pads by their centres of gravity; then the median of the offsets of the paired pads,
  // which a wrong pad or two cannot move.
  Xy cb, ct;
  for (const auto& p : bp) cb.x += p.x / static_cast<double>(bp.size()), cb.y += p.y / static_cast<double>(bp.size());
  for (const auto& p : t) ct.x += p.x / static_cast<double>(t.size()), ct.y += p.y / static_cast<double>(t.size());
  a.tx = cb.x - ct.x;
  a.ty = cb.y - ct.y;
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<char> taken(bp.size(), 0);
    std::vector<double> dx, dy;
    for (std::size_t i = 0; i < rp.size(); ++i) {
      a.to_board[i] = -1;
      const auto it = by_number.find(rp[i].number);
      if (it == by_number.end()) continue;
      double best = 0;
      int pick = -1;
      for (const int j : it->second) {
        if (taken[static_cast<std::size_t>(j)]) continue;
        const double d = std::hypot(bp[static_cast<std::size_t>(j)].x - (t[i].x + a.tx), bp[static_cast<std::size_t>(j)].y - (t[i].y + a.ty));
        if (pick < 0 || d < best) best = d, pick = j;
      }
      if (pick < 0) continue;
      taken[static_cast<std::size_t>(pick)] = 1;
      a.to_board[i] = pick;
      dx.push_back(bp[static_cast<std::size_t>(pick)].x - t[i].x);
      dy.push_back(bp[static_cast<std::size_t>(pick)].y - t[i].y);
    }
    if (dx.empty()) break;
    a.tx = median(dx);
    a.ty = median(dy);
  }
  a.matched = 0;
  for (std::size_t i = 0; i < rp.size(); ++i) {
    if (a.to_board[i] < 0) continue;
    ++a.matched;
    const auto& b = bp[static_cast<std::size_t>(a.to_board[i])];
    const double d = std::hypot(b.x - (t[i].x + a.tx), b.y - (t[i].y + a.ty));
    a.max_dev = std::max(a.max_dev, d);
    a.sum_dev += d;
  }
  return a;
}

// The better of two alignments: more pads paired; then the smaller worst deviation (within 5 nm they are equal: a symmetric pattern fits
// several ways); then the one that agrees with the side the footprint is on; then the smaller rotation.
bool better(const Alignment& a, const Alignment& b, bool back) {
  if (a.matched != b.matched) return a.matched > b.matched;
  if (std::fabs(a.max_dev - b.max_dev) > 5.0) return a.max_dev < b.max_dev;
  if (std::fabs(a.sum_dev - b.sum_dev) > 5.0 * std::max(1, a.matched)) return a.sum_dev < b.sum_dev;
  const int pa = (a.mirror != back ? 10 : 0) + a.rot / 90, pb = (b.mirror != back ? 10 : 0) + b.rot / 90;
  return pa < pb;
}

// Do the two pads' outlines agree in size? The datasheet pad is turned with the pattern; a quarter turn swaps its width and height.
struct SizeVerdict {
  bool ok = true;
  bool orientation = false;  // an angle that is not a multiple of 90 degrees apart: sizes cannot be compared as they are
  double ref_w = 0, ref_h = 0;  // the datasheet size in the board pad's own axes
};
SizeVerdict compare_sizes(const BoardPad& bp, const RefPad& rp, int rot_deg, bool mirror, double tol) {
  SizeVerdict v;
  const double bw = static_cast<double>(bp.p->size_x), bh = static_cast<double>(bp.p->size_y);
  v.ref_w = rp.w;
  v.ref_h = rp.h;
  const bool round_both = std::fabs(bw - bh) <= tol && std::fabs(rp.w - rp.h) <= tol;
  if (round_both) {
    v.ok = std::fabs(bw - rp.w) <= tol && std::fabs(bh - rp.h) <= tol;
    return v;
  }
  auto rel = [&](double ref_angle) {
    double d = std::fmod(std::fabs(bp.angle - ref_angle), 180.0);
    if (d < 0) d += 180.0;
    return d;
  };
  for (const double sgn : {mirror ? -1.0 : 1.0, mirror ? 1.0 : -1.0}) {  // the second only if the first is not on an axis
    const double d = rel(sgn * rp.angle + rot_deg);
    if (d <= 1.0 || d >= 179.0) {
      v.ok = std::fabs(bw - rp.w) <= tol && std::fabs(bh - rp.h) <= tol;
      return v;
    }
    if (std::fabs(d - 90.0) <= 1.0) {
      v.ref_w = rp.h;
      v.ref_h = rp.w;
      v.ok = std::fabs(bw - rp.h) <= tol && std::fabs(bh - rp.w) <= tol;
      return v;
    }
  }
  v.orientation = true;
  v.ok = false;
  return v;
}

FootprintIssue& add(FootprintResult& r, Severity s, const char* kind, const std::string& pad, const std::string& message, Point at, double actual = -1, double expected = -1) {
  FootprintIssue i;
  i.severity = s;
  i.kind = kind;
  i.pad = pad;
  i.message = message;
  i.at = at;
  i.actual_mm = actual;
  i.expected_mm = expected;
  r.issues.push_back(std::move(i));
  return r.issues.back();
}

std::string numbers(const std::vector<std::string>& v, std::size_t limit = 6) {
  std::string out;
  for (std::size_t i = 0; i < v.size() && i < limit; ++i) out += (i ? ", " : "") + pad_name(v[i]);
  if (v.size() > limit) out += ", ... (" + std::to_string(v.size()) + " in all)";
  return out;
}

void compare(const model::Board& b, const model::Footprint& fp, const FootprintRef& ref, const FootprintCheckOptions& opt, FootprintResult& r) {
  const auto bp = board_pads(b, fp);
  r.pads_board = static_cast<int>(bp.size());
  r.pads_ref = static_cast<int>(ref.pads.size());
  const double tol_pos = ref.position_tol_nm >= 0 ? ref.position_tol_nm : opt.position_tol_mm * 1e6;
  const double tol_size = ref.size_tol_nm >= 0 ? ref.size_tol_nm : opt.size_tol_mm * 1e6;
  const double tol_drill = ref.drill_tol_nm >= 0 ? ref.drill_tol_nm : opt.drill_tol_mm * 1e6;
  if (bp.empty()) {
    add(r, Severity::Error, "pattern", "", "the footprint has no pads, the datasheet land pattern has " + std::to_string(ref.pads.size()), fp.pos).label = "no pads";
    return;
  }
  Alignment best;
  bool have = false;
  for (const bool mirror : {false, true})
    for (const int rot_deg : {0, 90, 180, 270}) {
      Alignment a = align(bp, ref.pads, rot_deg, mirror);
      if (!have || better(a, best, fp.back)) best = std::move(a), have = true;
    }
  r.rotation = best.rot;
  r.mirrored = best.mirror;
  r.pads_matched = best.matched;
  if (best.matched == 0) {
    std::vector<std::string> mine, theirs;
    for (const auto& p : bp) mine.push_back(p.p->number);
    for (const auto& p : ref.pads) theirs.push_back(p.number);
    add(r, Severity::Error, "pattern", "", "no pad number of the footprint (" + numbers(mine) + ") is in the datasheet land pattern (" + numbers(theirs) + ")", fp.pos).label = "pad numbers differ";
    return;
  }
  if (best.mirror != fp.back)
    add(r, Severity::Error, "mirrored", "",
        fp.back ? "the footprint is on the back but its pads match the datasheet land pattern unmirrored: it was not flipped, or its pin numbers run the wrong way"
                : "the pads match the datasheet land pattern only mirrored: the pin numbers run the wrong way round",
        fp.pos)
        .label = "mirrored";
  std::vector<char> used(bp.size(), 0);
  std::vector<std::string> missing;
  for (std::size_t i = 0; i < ref.pads.size(); ++i) {
    const RefPad& rp = ref.pads[i];
    if (best.to_board[i] < 0) {
      missing.push_back(rp.number);
      continue;
    }
    const auto& bpad = bp[static_cast<std::size_t>(best.to_board[i])];
    used[static_cast<std::size_t>(best.to_board[i])] = 1;
    const model::Pad& p = *bpad.p;
    const Xy t = transform(rp, best.rot, best.mirror);
    const double dev = std::hypot(bpad.x - (t.x + best.tx), bpad.y - (t.y + best.ty));
    r.max_position_mm = std::max(r.max_position_mm, mm(dev));
    const std::string pn = pad_name(p.number);
    if (norm_type(p.type) != norm_type(rp.type)) {
      add(r, Severity::Error, "pad_type", p.number, "pad " + pn + " is a " + type_name(p.type) + ", the datasheet land pattern has a " + type_name(rp.type), p.pos).label = "pad " + pn + ": type";
      continue;
    }
    if (dev > tol_pos + 1.0)
      add(r, Severity::Error, "position", p.number,
          "pad " + pn + " is " + num(mm(dev)) + " mm from where the datasheet land pattern puts it (tolerance " + num(mm(tol_pos)) + " mm)", p.pos, mm(dev), mm(tol_pos))
          .label = "pad " + pn + ": " + num(mm(dev)) + " mm off";
    if (p.shape != model::PadShape::Custom && rp.shape != model::PadShape::Custom) {
      const SizeVerdict sv = compare_sizes(bpad, rp, best.rot, best.mirror, tol_size);
      if (sv.orientation)
        add(r, Severity::Error, "orientation", p.number, "pad " + pn + " is turned to an angle the datasheet land pattern does not have: its size cannot be compared", p.pos).label = "pad " + pn + ": angle";
      else if (!sv.ok)
        add(r, Severity::Error, "size", p.number,
            "pad " + pn + " is " + num(mm(static_cast<double>(p.size_x))) + " x " + num(mm(static_cast<double>(p.size_y))) + " mm, the datasheet land pattern has " + num(mm(sv.ref_w)) + " x " +
                num(mm(sv.ref_h)) + " mm (tolerance " + num(mm(tol_size)) + " mm)",
            p.pos, mm(static_cast<double>(std::max(p.size_x, p.size_y))), mm(std::max(sv.ref_w, sv.ref_h)))
            .label = "pad " + pn + ": " + num(mm(static_cast<double>(p.size_x))) + "x" + num(mm(static_cast<double>(p.size_y))) + " vs " + num(mm(sv.ref_w)) + "x" + num(mm(sv.ref_h)) + " mm";
    }
    const double b0 = static_cast<double>(std::min(p.drill_x, p.drill_y)), b1 = static_cast<double>(std::max(p.drill_x, p.drill_y));
    const double r0 = std::min(rp.drill_w, rp.drill_h), r1 = std::max(rp.drill_w, rp.drill_h);
    if ((b1 > 0) != (r1 > 0) || (b1 > 0 && (std::fabs(b0 - r0) > tol_drill || std::fabs(b1 - r1) > tol_drill)))
      add(r, Severity::Error, "drill", p.number,
          "pad " + pn + " has " + (b1 > 0 ? "a " + num(mm(b1)) + " mm drill" : std::string("no drill")) + ", the datasheet land pattern " + (r1 > 0 ? "a " + num(mm(r1)) + " mm drill" : std::string("none")) +
              " (tolerance " + num(mm(tol_drill)) + " mm)",
          p.pos, mm(b1), mm(r1))
          .label = "pad " + pn + ": drill " + (b1 > 0 ? num(mm(b1)) : std::string("none")) + " vs " + (r1 > 0 ? num(mm(r1)) : std::string("none")) + " mm";
  }
  // The count: without the pegs (unnumbered non-plated holes) the datasheet does not show, which have a warning of their own.
  std::size_t pegs = 0;
  for (std::size_t j = 0; j < bp.size(); ++j)
    if (!used[j] && bp[j].p->type == model::PadType::NpThruHole && bp[j].p->number.empty()) ++pegs;
  if (bp.size() - pegs != ref.pads.size())
    add(r, Severity::Error, "pad_count", "", "the footprint has " + std::to_string(bp.size() - pegs) + " pads, the datasheet land pattern " + std::to_string(ref.pads.size()), fp.pos,
        static_cast<double>(bp.size() - pegs), static_cast<double>(ref.pads.size()))
        .label = "pad count " + std::to_string(bp.size() - pegs) + " vs " + std::to_string(ref.pads.size());
  if (!missing.empty()) add(r, Severity::Error, "missing_pad", "", "datasheet pad " + numbers(missing) + " " + (missing.size() == 1 ? "is" : "are") + " not on the footprint", fp.pos).label = "missing pad " + numbers(missing, 3);
  for (std::size_t j = 0; j < bp.size(); ++j) {
    if (used[j]) continue;
    const model::Pad& p = *bp[j].p;
    const bool mechanical = p.type == model::PadType::NpThruHole && p.number.empty();
    add(r, mechanical ? Severity::Warning : Severity::Error, "extra_pad", p.number,
        mechanical ? "a non-plated hole at the footprint is not in the datasheet land pattern: check that it is a locating peg of the package" : "pad " + pad_name(p.number) + " is not in the datasheet land pattern",
        p.pos)
        .label = mechanical ? "extra hole" : "extra pad " + pad_name(p.number);
  }
}

// The footprint's manufacturer part numbers: the properties that are called MPN or something like it.
std::vector<std::string> mpns_of(const model::Footprint& fp, const std::set<std::string>& keys) {
  std::vector<std::string> out;
  for (const auto& [k, v] : fp.properties)
    if (!v.empty() && keys.count(key_norm(k))) out.push_back(v);
  return out;
}

bool matches(const FootprintRef& ref, const model::Footprint& fp, const std::vector<std::string>& mpn) {
  auto any_ci = [](const std::vector<std::string>& pats, const std::vector<std::string>& values) {
    return std::any_of(pats.begin(), pats.end(), [&](const std::string& p) { return std::any_of(values.begin(), values.end(), [&](const std::string& v) { return wild_ci(p, v); }); });
  };
  if (!ref.mpn.empty() && !any_ci(ref.mpn, mpn)) return false;
  if (!ref.value.empty() && !any_ci(ref.value, {fp.value})) return false;
  if (!ref.lib_id.empty() && !any_wild(ref.lib_id, fp.lib_id)) return false;
  if (!ref.ref.empty() && !any_wild(ref.ref, fp.reference)) return false;
  return true;
}

nlohmann::json ref_json(const FootprintResult& r) {
  nlohmann::json j;
  j["id"] = r.ref_id;
  j["brand"] = r.brand;
  j["part"] = r.part;
  j["document"] = r.document;
  if (!r.revision.empty()) j["revision"] = r.revision;
  if (r.page > 0) j["page"] = r.page;
  if (!r.figure.empty()) j["figure"] = r.figure;
  if (!r.url.empty()) j["url"] = r.url;
  if (r.other_matches > 0) j["other_matches"] = r.other_matches;
  return j;
}

const char* status_name(FootprintStatus s) {
  switch (s) {
    case FootprintStatus::Ok: return "ok";
    case FootprintStatus::Mismatch: return "mismatch";
    case FootprintStatus::Unverified: return "unverified";
    case FootprintStatus::Untrusted: return "untrusted";
    case FootprintStatus::Skipped: return "skipped";
  }
  return "?";
}
const char* severity_name(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }
double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

}  // namespace

FootprintCheckReport check_footprints(const model::Board& b, const RefLibrary& lib, const FootprintCheckOptions& opt) {
  FootprintCheckReport rep;
  rep.options = opt;
  rep.references = static_cast<int>(lib.refs.size());
  rep.trusted_source = lib.trusted.source;
  rep.trust_all = lib.trusted.trust_all;
  if (lib.trusted.trust_all)
    rep.notes.push_back("trust-all: every reference is used whatever its brand; nobody vouched for them");
  else if (lib.trusted.groups.empty())
    rep.notes.push_back("no trusted brand list: no reference can be used (give --trusted-brands FILE, put trusted_brands.txt beside the references, or --trust-all)");
  if (lib.refs.empty()) rep.notes.push_back("the reference library has no references: nothing can be verified");
  std::set<std::string> mpn_keys{"mpn", "manufacturerpartnumber", "mfrpartnumber", "mfgpartnumber", "partnumber", "mfrpn", "manufacturerpn", "mfrpart"};
  for (const auto& f : opt.mpn_fields) mpn_keys.insert(key_norm(f));
  std::set<std::string> ambiguous;  // "REF: a, b" notes, once each
  for (std::size_t fi = 0; fi < b.footprints.size(); ++fi) {
    const model::Footprint& fp = b.footprints[fi];
    FootprintResult r;
    r.footprint = static_cast<int>(fi);
    r.ref = fp.reference;
    r.lib_id = fp.lib_id;
    r.value = fp.value;
    r.at = fp.pos;
    r.mpn = mpns_of(fp, mpn_keys);
    const bool wanted = opt.only.empty() || any_wild(opt.only, fp.reference);
    if (fp.pads.empty() || fp.reference.empty() || !wanted) {
      r.status = FootprintStatus::Skipped;
      ++rep.skipped;
      rep.results.push_back(std::move(r));
      continue;
    }
    ++rep.checked;
    std::vector<std::size_t> hits;
    for (std::size_t k = 0; k < lib.refs.size(); ++k)
      if (matches(lib.refs[k], fp, r.mpn)) hits.push_back(k);
    // The most specific record first, then library order; the first one whose brand is trusted is used.
    std::stable_sort(hits.begin(), hits.end(), [&](std::size_t a, std::size_t c) { return lib.refs[a].specificity() > lib.refs[c].specificity(); });
    const FootprintRef* use = nullptr;
    const FootprintRef* first = hits.empty() ? nullptr : &lib.refs[hits[0]];
    for (const std::size_t k : hits)
      if (lib.trusted.trusts(lib.refs[k].brand)) {
        use = &lib.refs[k];
        break;
      }
    auto describe = [&](const FootprintRef& ref) {
      r.ref_id = ref.id;
      r.brand = ref.brand;
      r.part = ref.part;
      r.document = ref.document;
      r.revision = ref.revision;
      r.figure = ref.figure;
      r.url = ref.url;
      r.page = ref.page;
      r.other_matches = static_cast<int>(hits.size()) - 1;
    };
    const bool required = any_wild(opt.require, fp.reference);
    if (use != nullptr) {
      describe(*use);
      if (hits.size() > 1) {
        std::string s = fp.reference + " matches " + std::to_string(hits.size()) + " references, used " + use->id + ":";
        for (const std::size_t k : hits) s += " " + lib.refs[k].id;
        ambiguous.insert(s);
      }
      compare(b, fp, *use, opt, r);
      r.status = r.errors() > 0 ? FootprintStatus::Mismatch : FootprintStatus::Ok;
      if (r.status == FootprintStatus::Ok) ++rep.ok;
      else ++rep.mismatched;
    } else if (first != nullptr) {
      describe(*first);
      r.status = FootprintStatus::Untrusted;
      ++rep.untrusted;
      add(r, required ? Severity::Error : Severity::Warning, "untrusted_source", "",
          "the datasheet reference " + first->id + " is from '" + first->brand + "', which is not on the trusted list: the footprint is not verified", fp.pos)
          .label = "reference not trusted";
    } else {
      r.status = FootprintStatus::Unverified;
      ++rep.unverified;
      if (required) add(r, Severity::Error, "no_reference", "", "no datasheet reference for this footprint (a reference is required for " + fp.reference + ")", fp.pos).label = "no reference";
    }
    rep.errors += r.errors();
    rep.warnings += r.warnings();
    rep.results.push_back(std::move(r));
  }
  for (const auto& s : ambiguous) rep.notes.push_back(s);
  return rep;
}

nlohmann::json footprint_check_json(const model::Board& b, const FootprintCheckReport& r) {
  using nlohmann::json;
  (void)b;
  json j;
  j["summary"] = {{"checked", r.checked}, {"ok", r.ok}, {"mismatch", r.mismatched}, {"unverified", r.unverified}, {"untrusted", r.untrusted}, {"skipped", r.skipped}, {"errors", r.errors}, {"warnings", r.warnings}};
  j["options"] = {{"position_tol_mm", r.options.position_tol_mm}, {"size_tol_mm", r.options.size_tol_mm}, {"drill_tol_mm", r.options.drill_tol_mm}, {"only", r.options.only}, {"require", r.options.require}};
  j["references"] = r.references;
  j["trusted_brands"] = {{"source", r.trusted_source}, {"trust_all", r.trust_all}};
  j["notes"] = r.notes;
  j["footprints"] = json::array();
  for (const auto& f : r.results) {
    json o;
    o["ref"] = f.ref;
    o["lib_id"] = f.lib_id;
    o["value"] = f.value;
    if (!f.mpn.empty()) o["mpn"] = f.mpn;
    o["at"] = {r3(mm(static_cast<double>(f.at.x))), r3(mm(static_cast<double>(f.at.y)))};
    o["status"] = status_name(f.status);
    if (!f.ref_id.empty()) o["reference"] = ref_json(f);
    if (f.status == FootprintStatus::Ok || f.status == FootprintStatus::Mismatch) {
      o["alignment"] = {{"rotation", f.rotation}, {"mirrored", f.mirrored}};
      o["pads"] = {{"board", f.pads_board}, {"datasheet", f.pads_ref}, {"matched", f.pads_matched}};
      o["max_position_mm"] = r3(f.max_position_mm);
    }
    o["issues"] = json::array();
    for (const auto& i : f.issues) {
      json ij{{"severity", severity_name(i.severity)}, {"kind", i.kind}, {"message", i.message}, {"label", i.label}, {"at", {r3(mm(static_cast<double>(i.at.x))), r3(mm(static_cast<double>(i.at.y)))}}};
      if (!i.pad.empty()) ij["pad"] = i.pad;
      if (i.actual_mm >= 0) ij["actual_mm"] = r3(i.actual_mm);
      if (i.expected_mm >= 0) ij["expected_mm"] = r3(i.expected_mm);
      o["issues"].push_back(std::move(ij));
    }
    j["footprints"].push_back(std::move(o));
  }
  return j;
}

std::string footprint_check_message(const FootprintCheckReport& r) {
  using nlohmann::json;
  json m;
  m["type"] = "footprint_check";
  m["checked"] = r.checked;
  m["ok"] = r.ok;
  m["mismatch"] = r.mismatched;
  m["unverified"] = r.unverified;
  m["untrusted"] = r.untrusted;
  m["errors"] = r.errors;
  m["warnings"] = r.warnings;
  m["footprints"] = json::array();
  for (const auto& f : r.results) {
    if (f.issues.empty()) continue;  // what is fine, or merely has no reference, is not worth a marker
    json o{{"ref", f.ref}, {"lib_id", f.lib_id}, {"status", status_name(f.status)}, {"p", {f.at.x, f.at.y}}};
    if (!f.ref_id.empty()) o["datasheet"] = ref_json(f);
    o["issues"] = json::array();
    for (const auto& i : f.issues) o["issues"].push_back({{"severity", severity_name(i.severity)}, {"kind", i.kind}, {"pad", i.pad}, {"text", i.message}, {"label", i.label}, {"p", {i.at.x, i.at.y}}});
    m["footprints"].push_back(std::move(o));
  }
  return m.dump();
}

std::vector<std::string> footprint_check_lines(const FootprintCheckReport& r, bool all) {
  std::vector<std::string> out;
  char buf[256];
  for (const auto& f : r.results) {
    if (f.status == FootprintStatus::Skipped) continue;
    const bool problem = f.status == FootprintStatus::Mismatch || !f.issues.empty();
    if (!problem && !all) continue;
    std::string src;
    if (!f.ref_id.empty()) {
      src = f.brand + " " + f.part + " (" + f.document + (f.revision.empty() ? "" : " rev " + f.revision) + (f.page > 0 ? ", p." + std::to_string(f.page) : "") + ")";
    }
    std::string head = f.ref + "  " + f.lib_id + (f.value.empty() ? "" : "  " + f.value) + "  ";
    switch (f.status) {
      case FootprintStatus::Ok: head += "ok vs " + src; break;
      case FootprintStatus::Mismatch: head += "MISMATCH vs " + src; break;
      case FootprintStatus::Untrusted: head += "UNTRUSTED reference " + src; break;
      default: head += "no reference"; break;
    }
    out.push_back(head);
    // At most six of a kind: a footprint that is the wrong pattern altogether does not need every pad listed.
    std::map<std::string, int> shown;
    std::map<std::string, int> total;
    for (const auto& i : f.issues) ++total[i.kind];
    for (const auto& i : f.issues) {
      if (++shown[i.kind] > 6) continue;
      std::snprintf(buf, sizeof buf, "    %-7s %-12s ", severity_name(i.severity), i.kind.c_str());
      out.push_back(std::string(buf) + i.message);
    }
    for (const auto& [kind, n] : total)
      if (n > 6) out.push_back("    ... and " + std::to_string(n - 6) + " more " + kind + " issues (--json has them all)");
  }
  for (const auto& n : r.notes) out.push_back("note: " + n);
  std::snprintf(buf, sizeof buf, "%d footprints checked against %d references: %d ok, %d mismatch, %d untrusted, %d without a reference; %d errors, %d warnings", r.checked, r.references, r.ok,
                r.mismatched, r.untrusted, r.unverified, r.errors, r.warnings);
  out.push_back(buf);
  return out;
}

}  // namespace tmk::analysis
