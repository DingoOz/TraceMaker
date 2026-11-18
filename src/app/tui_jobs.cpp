// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/tui_jobs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace tmk::app::tui {

namespace fs = std::filesystem;
using Spans = std::vector<Span>;
using draw::border;
using draw::boxed;
using draw::fit;

namespace {

// Measured thresholds the advice quotes; each names its record.
// M26 F9 (bench/records/m26/f9): two-layer srj20 boards whose connections' straight lines cross this often per connection
// were never routed clean (8 boards); the router's own hint uses the same value (route_job.cpp kTwoLayerCrossings).
constexpr double kTwoLayerCrossings = 11.5;

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string duration(double s) {
  if (s < 0) s = 0;
  if (s < 90) return fmt("%.0f s", s);
  if (s < 5400) return fmt("%.0f min", s / 60);
  if (s < 3 * 86400) return fmt("%.1f h", s / 3600);
  return fmt("%.0f d", s / 86400);
}

// "N of M" at the start of a text such as "85 of 87 routed (best variant)".
bool n_of_m(const std::string& s, int& n, int& m) { return std::sscanf(s.c_str(), "%d of %d", &n, &m) == 2; }

bool process_alive(int pid, long start) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  if (!f) return false;
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto rp = s.rfind(')');
  if (rp == std::string::npos) return false;
  std::size_t i = rp + 2;
  for (int field = 3; field < 22 && i < s.size(); ++field) i = s.find(' ', i) + 1;
  return start <= 0 || std::atol(s.c_str() + i) == start;  // the same pid given to a later process is not this job
}

// Eight levels of a block, for a history in one line.
std::string spark(const std::vector<int>& v, std::size_t w, int top) {
  static const char* const bars[] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                     "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};
  std::string out;
  if (v.empty() || w == 0 || top <= 0) return out;
  const std::size_t n = std::min(w, v.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t a = i * v.size() / n, b = std::max(a + 1, (i + 1) * v.size() / n);
    int best = 0;
    for (std::size_t k = a; k < b; ++k) best = std::max(best, v[k]);
    out += bars[std::clamp(best * 7 / top, 0, 7)];
  }
  return out;
}

}  // namespace

std::string Job::field(const std::string& key) const {
  for (const auto& [k, v] : fields)
    if (k == key) return v;
  return {};
}

std::optional<std::string> Job::option(const std::string& spelling) const {
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == spelling && i + 1 < argv.size()) return argv[i + 1];
    if (argv[i].rfind(spelling + "=", 0) == 0) return argv[i].substr(spelling.size() + 1);
  }
  return std::nullopt;
}

bool Job::has(const std::string& spelling) const {
  for (const auto& a : argv)
    if (a == spelling || a.rfind(spelling + "=", 0) == 0) return true;
  return false;
}

std::string Job::path(const std::string& p) const {
  if (p.empty() || fs::path(p).is_absolute() || cwd.empty()) return p;
  return (fs::path(cwd) / p).lexically_normal().string();
}

std::string Job::board() const {
  if (const std::string b = field("board"); !b.empty()) return b;
  for (std::size_t i = 0; i < argv.size(); ++i)
    if (argv[i].size() > 10 && argv[i].substr(argv[i].size() - 10) == ".kicad_pcb" && (i == 0 || argv[i - 1].front() != '-')) return argv[i];
  return argv.empty() ? std::string() : argv.front();
}

int Job::routed() const {
  if (summary.is_object() && summary.contains("routed")) return summary["routed"].get<int>();
  int n = 0, m = 0;
  if (n_of_m(field("result"), n, m)) return n;
  int best = 0;
  for (const auto& s : slots) best = std::max(best, s.routed);
  return best;
}

int Job::total() const {
  if (summary.is_object() && summary.contains("connections")) return summary["connections"].get<int>();
  int n = 0, m = 0;
  if (n_of_m(field("result"), n, m)) return m;
  int best = 0;
  for (const auto& s : slots) best = std::max(best, s.total);
  if (best == 0 && !h_total.empty()) best = *std::max_element(h_total.begin(), h_total.end());
  return best;
}

double Job::budget_s() const {
  const std::string b = field("budget");
  for (const char* key : {"--time-total ", "--time "}) {
    const auto at = b.find(key);
    if (at != std::string::npos) return std::atof(b.c_str() + at + std::strlen(key));
  }
  return 0;
}

double Job::stalled_s() const {
  if (h_routed.empty()) return 0;
  const int best = *std::max_element(h_routed.begin(), h_routed.end());
  std::size_t first = 0;
  while (h_routed[first] < best) ++first;
  return static_cast<double>(h_routed.size() - 1 - first) * h_step_s;
}

Job parse_job(const nlohmann::json& j, const std::function<bool(int, long)>& alive) {
  Job o;
  o.pid = j.value("pid", 0);
  o.state = j.value("state", std::string("running"));
  if (j.contains("exit") && j["exit"].is_number()) o.exit_code = j["exit"].get<int>();
  o.started = j.value("started", 0.0);
  o.updated = j.value("updated", 0.0);
  o.elapsed_s = j.value("elapsed_s", 0.0);
  o.cpu_s = j.value("cpu_s", 0.0);
  o.cwd = j.value("cwd", std::string());
  if (j.contains("argv"))
    for (std::size_t i = 2; i < j["argv"].size(); ++i) o.argv.push_back(j["argv"][i].get<std::string>());  // after "tracemaker route"
  if (j.contains("fields"))
    for (const auto& f : j["fields"]) o.fields.emplace_back(f[0].get<std::string>(), f[1].get<std::string>());
  if (j.contains("slots"))
    for (const auto& s : j["slots"])
      o.slots.push_back({s.value("name", std::string()), s.value("routed", 0), s.value("total", 0), s.value("phase", 0), s.value("runs", 0)});
  if (j.contains("history")) {
    const auto& h = j["history"];
    o.h_step_s = std::max(1, h.value("step_s", 1));
    if (h.contains("routed")) o.h_routed = h["routed"].get<std::vector<int>>();
    if (h.contains("total")) o.h_total = h["total"].get<std::vector<int>>();
  }
  if (j.contains("log")) o.log = j["log"].get<std::vector<std::string>>();
  const long start = j.contains("proc_start") && j["proc_start"].is_number() ? j["proc_start"].get<long>() : 0;
  if (o.state == "running" && alive && !alive(o.pid, start)) o.state = "gone";
  return o;
}

std::string live_folder() {
  if (const char* d = std::getenv("TM_LIVE_DIR"); d && *d) return d;
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return (fs::path(x) / "tracemaker" / "live").string();
  if (const char* h = std::getenv("HOME"); h && *h) return (fs::path(h) / ".cache" / "tracemaker" / "live").string();
  return {};
}

std::vector<Job> read_jobs(const std::string& dir, double max_age_s, std::size_t limit) {
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json") continue;
    std::error_code e2;
    const auto t = fs::last_write_time(e.path(), e2);
    if (e2 || std::chrono::duration<double>(now - t).count() > max_age_s) continue;  // most files are old: skip them unread
    files.emplace_back(t, e.path());
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  std::vector<Job> jobs;
  for (const auto& [t, p] : files) {
    if (jobs.size() >= limit) break;
    std::ifstream f(p);
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("argv") || j["argv"].size() < 2 || j["argv"][1] != "route") continue;
    Job job = parse_job(j, process_alive);
    job.file = p.string();
    if (const auto js = job.option("--json")) {  // the summary, once this run has written it
      const fs::path sp = job.path(*js);
      std::error_code e3;
      const auto st = fs::last_write_time(sp, e3);
      if (!e3 && job.state != "running" &&
          static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(st).time_since_epoch()).count()) + 5 >= job.started) {
        std::ifstream sf(sp);
        job.summary = nlohmann::json::parse(sf, nullptr, false);
        if (job.summary.is_discarded()) job.summary = nullptr;
      }
    }
    jobs.push_back(std::move(job));
  }
  std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return (a.state == "running") > (b.state == "running"); });
  return jobs;
}

std::vector<Advice> advise(const Job& j) {
  std::vector<Advice> out;
  const int total = j.total(), routed = j.routed(), open = total > 0 ? total - routed : -1;
  const bool running = j.state == "running";
  const std::string output = j.path(j.option("-o").value_or(j.option("--output").value_or("")));
  // A rerun that starts from this result writes elsewhere, so the board it starts from stays as it is.
  auto next_output = [&] {
    if (output.empty()) return std::string();
    const fs::path p(output);
    std::string stem = p.stem().string();
    return (p.parent_path() / (stem + (stem.size() > 5 && stem.substr(stem.size() - 5) == "-next" ? "2" : "-next") + p.extension().string())).string();
  };

  if (j.state == "gone") {
    out.push_back({"The process is gone without a final state (stopped or crashed) at: " + j.field("stage"),
                   "its live file still says running; the board it was writing may be missing or old", {}, {}});
    return out;
  }
  if (!running && j.exit_code != 0 && j.exit_code != 3 && total == 0) {  // 3: routed, but not complete
    out.push_back({"The run failed before routing; its last lines are in the log below", "exit code " + std::to_string(j.exit_code), {}, {}});
    return out;
  }
  if (running) {
    const double stall = j.stalled_s(), budget = j.budget_s();
    if (open > 0 && stall >= std::max(600.0, 0.25 * j.elapsed_s))
      out.push_back({fmt("No gain for %s: best %d of %d routed", duration(stall).c_str(), routed, total),
                     budget > 0 ? fmt("from the live history; the run stops at its limit in %s", duration(budget - j.elapsed_s).c_str())
                                : std::string("from the live history; the run has no time limit"),
                     {}, {}});
    if (!j.has("--json") && !output.empty())
      out.push_back({"Add --json on the next run, so this screen can show the router's diagnosis when it ends",
                     "without it only the live log is left to read", {{"--json", output + ".json"}}, {}});
    return out;
  }
  if (open == 0) {
    out.push_back({"Complete: every connection is routed", "check it with KiCad's rules: tracemaker drc " + (output.empty() ? std::string("OUTPUT") : output), {}, {}});
    return out;
  }
  if (open < 0) return out;

  const nlohmann::json& s = j.summary;
  const bool have = s.is_object();
  const int layers = have ? s.value("copper_layers", 0) : 0;
  const long crossings = have ? s.value("line_crossings", 0L) : 0;
  const bool time_limited = have && s.value("time_limited", false);
  const bool relax = j.has("--relax");
  const bool layered = j.has("--try-layers") || j.has("--copper-layers");

  if (layers == 2 && !layered && total > 0 && crossings > 0 && static_cast<double>(crossings) / total >= kTwoLayerCrossings)
    out.push_back({"Let the router add two inner layers if two stay open: --try-layers 2,4",
                   fmt("%.1f crossings per connection; no two-layer srj20 board above %.1f routed clean (8 boards), and sample152 "
                       "routed 87/87 at 4 layers in 22 s (bench/records/m26/layers4)",
                       static_cast<double>(crossings) / total, kTwoLayerCrossings),
                   {{"--try-layers", "2,4"}}, {}});
  if (!relax)
    out.push_back({"Add the relax rescue and an hour: --relax --time-total 3600",
                   "srj20: one-hour relax runs closed 50 of the 69 boards the default route left open", {{"--relax", ""}, {"--time-total", "3600"}}, {}});
  if (relax && time_limited && !j.has("--warm-pcb") && !output.empty())
    out.push_back({fmt("Go on from these %d routes: --warm-pcb this result, written to a new file", routed),
                   "the run stopped at its time limit; a warm start keeps the routed copper instead of starting again",
                   {{"--warm-pcb", output}, {"-o", next_output()}}, {}});
  if (relax && !j.has("--ramp-from") && open <= 20 && !output.empty())
    out.push_back({"Ramp the clearance from a warm start: --ramp-from 0.06 --ramp-step 0.02 --ramp-rounds 3",
                   "srj20: the ramp closed 4 of the 15 boards still open after the long relax runs; from a warm start it never "
                   "returns fewer routes (D191 F8)",
                   {{"--warm-pcb", output}, {"-o", next_output()}, {"--ramp-from", "0.06"}, {"--ramp-step", "0.02"}, {"--ramp-rounds", "3"}}, {}});
  if (relax && time_limited) {
    const double t = j.budget_s();
    out.push_back({"Another seed or more time: --seed N+1, or a longer --time-total",
                   "srj20: longer runs and other seeds closed 4 more boards after the one-hour runs",
                   {{"--seed", std::to_string(std::atoi(j.option("--seed").value_or("0").c_str()) + 1)},
                    {"--time-total", fmt("%.0f", t > 0 ? 2 * t : 7200)}},
                   {}});
  }
  // The router's own diagnosis of the open pins (doc 20 §2.5): its "next" lines.
  std::vector<std::string> next;
  if (have && s.contains("why_open") && s["why_open"].contains("next"))
    next = s["why_open"]["next"].get<std::vector<std::string>>();
  else
    for (const auto& l : j.log)
      if (const auto at = l.find("next: "); at != std::string::npos && l.find_first_not_of(' ') == at) next.push_back(l.substr(at + 6));
  for (const auto& n : next) out.push_back({"Router: " + n, "the router's proof of why the pins are open (why open)", {}, {}});
  if (!have && !j.has("--json"))
    out.push_back({"Add --json on the next run for the full diagnosis", "this run wrote no summary", {{"--json", output.empty() ? "out.json" : output + ".json"}}, {}});
  return out;
}

JobsModel::JobsModel(Loader load) : load_(std::move(load)) {
  now = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) / 1000;
  reload();
}

void JobsModel::reload() {
  const std::string keep = sel_ < jobs_.size() ? jobs_[sel_].file : std::string();
  jobs_ = load_ ? load_(all_) : std::vector<Job>{};
  sel_ = 0;
  for (std::size_t i = 0; i < jobs_.size(); ++i)
    if (jobs_[i].file == keep) sel_ = i;
  select_changed();
}

void JobsModel::select_changed() { advice_ = sel_ < jobs_.size() ? advise(jobs_[sel_]) : std::vector<Advice>{}; }

const Job* JobsModel::selected() const { return sel_ < jobs_.size() ? &jobs_[sel_] : nullptr; }

Action JobsModel::press(int k) {
  status_.clear();
  chosen_ = -1;
  switch (k) {
    case key::up:
    case 'k':
      if (sel_ > 0) --sel_, select_changed();
      break;
    case key::down:
    case 'j':
      if (sel_ + 1 < jobs_.size()) ++sel_, select_changed();
      break;
    case key::home:
    case 'g':
      sel_ = 0, select_changed();
      break;
    case key::end:
    case 'G':
      if (!jobs_.empty()) sel_ = jobs_.size() - 1, select_changed();
      break;
    case 'a':
      all_ = !all_;
      reload();
      status_ = all_ ? "showing the jobs of the last week" : "showing running jobs and those of the last day";
      break;
    case 'r':
      reload();
      break;
    case 'e':
    case key::enter:
      if (selected()) return Action::edit;
      break;
    case 'o':
    case key::esc:
      return Action::options;
    case 'q':
    case key::ctrl_c:
      return Action::quit;
    default:
      if (k >= '1' && k <= '9') {
        const std::size_t n = static_cast<std::size_t>(k - '1');
        if (n < advice_.size() && (!advice_[n].set.empty() || !advice_[n].unset.empty())) {
          chosen_ = static_cast<int>(n);
          return Action::edit;
        }
        status_ = n < advice_.size() ? "advice " + std::to_string(n + 1) + " changes no option" : "no advice " + std::to_string(n + 1);
      }
      break;
  }
  return Action::none;
}

std::vector<Line> JobsModel::render(int rows, int cols) const {
  const std::size_t w = static_cast<std::size_t>(std::max(cols, 40)), h = static_cast<std::size_t>(std::max(rows, 12));
  const std::size_t inner = w - 2;
  std::vector<Line> out;
  int running = 0;
  for (const Job& j : jobs_) running += j.state == "running";
  out.push_back(border("\xe2\x94\x8c", "\xe2\x94\x90", {{"tracemaker jobs", Style::title}},
                       {{fmt("%d running \xc2\xb7 %zu shown (%s)", running, jobs_.size(), all_ ? "last week" : "last day"), Style::dim}}, w));

  // The list: a third of the screen at most.
  const std::size_t list_n = std::max<std::size_t>(1, std::min(jobs_.size(), std::max<std::size_t>(3, h / 4)));
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + list_n) top_ = sel_ + 1 - list_n;
  const std::size_t board_w = inner > 60 ? inner - 52 : 8;
  out.push_back(boxed({{"   " + fit("state", 8, true) + fit("board", board_w, true) + " " + fit("routed", 10, true) + fit("open", 6, true) +
                            fit("time", 9, true) + "started",
                        Style::dim}},
                      w));
  for (std::size_t r = 0; r < list_n; ++r) {
    if (top_ + r >= jobs_.size()) {
      out.push_back(boxed({{jobs_.empty() && r == 0 ? "   no route jobs here yet (every `tracemaker route` run shows up)" : "", Style::dim}}, w));
      continue;
    }
    const Job& j = jobs_[top_ + r];
    const bool cur = top_ + r == sel_;
    const int t = j.total(), rt = j.routed();
    const Style st = j.state == "running" ? Style::accent : (j.state == "gone" ? Style::error : (t > 0 && rt == t ? Style::ok : Style::warn));
    const fs::path b(j.board());
    const std::string name = b.parent_path().filename().string() + "/" + b.filename().string();
    out.push_back(boxed({{cur ? " \xe2\x96\xb8 " : "   ", Style::accent},
                         {fit(j.state, 8, true), st},
                         {fit(name, board_w, true) + " ", Style::label},
                         {fit(t > 0 ? fmt("%d/%d", rt, t) : "-", 10, true), Style::plain},
                         {fit(t > 0 ? std::to_string(t - rt) : "", 6, true), t > 0 && rt < t ? Style::warn : Style::ok},
                         {fit(duration(j.elapsed_s), 9, true), Style::plain},
                         {duration(now - j.started) + " ago", Style::dim}},
                        w, cur));
  }

  const Job* j = selected();
  // Detail: progress (3 lines), options, advice, log; what is left goes to the log.
  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", {{"Progress", Style::heading}},
                       {{j ? fmt("pid %d \xc2\xb7 %s", j->pid, draw::short_path(j->cwd).c_str()) : std::string(), Style::dim}}, w));
  std::vector<Spans> progress(3);
  if (j) {
    const int t = j->total(), rt = j->routed();
    progress[0] = {{" board ", Style::dim}, {draw::short_path(j->path(j->board())), Style::label}};
    std::string stage = j->field("stage"), round = j->field("round");
    progress[1] = {{" stage ", Style::dim}, {stage.empty() ? j->state : stage, Style::plain}};
    if (!round.empty()) progress[1].push_back({"  \xc2\xb7  round " + round, Style::dim});
    Spans& left = progress[2];
    left = {{" left  ", Style::dim}};
    if (t > 0)
      left.push_back({rt == t ? std::string("nothing: complete") : fmt("%d of %d connections open", t - rt, t), rt == t ? Style::ok : Style::warn});
    const double budget = j->budget_s();
    if (j->state == "running" && budget > 0)
      left.push_back({fmt("  \xc2\xb7  %s of %s used, %s left", duration(j->elapsed_s).c_str(), duration(budget).c_str(), duration(budget - j->elapsed_s).c_str()), Style::plain});
    else
      left.push_back({"  \xc2\xb7  " + duration(j->elapsed_s) + (j->state == "running" ? " so far" : ""), Style::plain});
    int done = 0;
    for (const auto& s : j->slots) done += s.phase == 5;
    if (!j->slots.empty()) left.push_back({fmt("  \xc2\xb7  %zu variants, %d finished", j->slots.size(), done), Style::dim});
    if (!j->h_routed.empty() && t > 0 && inner > 60) {
      progress[0].push_back({"   ", Style::plain});
      progress[0].push_back({spark(j->h_routed, std::min<std::size_t>(30, inner / 4), t), Style::accent});
    }
  }
  for (Spans& sp : progress) out.push_back(boxed(std::move(sp), w));

  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", {{"Options", Style::heading}}, {{"e edits them for a rerun", Style::dim}}, w));
  std::string opts;
  if (j)
    for (std::size_t i = 0; i < j->argv.size(); ++i) opts += (opts.empty() ? "" : " ") + shell_quote(j->argv[i]);
  for (const auto& l : draw::wrap(opts.empty() ? "-" : opts, inner - 2, 2)) out.push_back(boxed({{" " + l, Style::code}}, w));

  // Advice: two lines each (what, then why), as many as fit with a few lines of log below.
  const std::size_t fixed_tail = 4;  // status, keys, bottom border, log rule
  const std::size_t used = out.size() + 1 + fixed_tail;
  const std::size_t room = h > used + 2 ? h - used - 2 : 0;  // at least two lines of log
  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", {{"Advice", Style::heading}}, {{"1-9 applies one to a rerun", Style::dim}}, w));
  std::size_t shown = 0;
  for (std::size_t i = 0; i < advice_.size() && shown + 2 <= std::max<std::size_t>(room, 2); ++i) {
    const Advice& a = advice_[i];
    const bool applies = !a.set.empty() || !a.unset.empty();
    out.push_back(boxed({{applies ? fmt(" %zu ", i + 1) : " \xc2\xb7 ", Style::key}, {a.text, applies ? Style::warn : Style::plain}}, w));
    out.push_back(boxed({{"   " + a.why, Style::dim}}, w));
    shown += 2;
  }
  if (advice_.empty()) {
    out.push_back(boxed({{j ? "   nothing to suggest yet" : "", Style::dim}}, w));
    ++shown;
  }

  out.push_back(border("\xe2\x94\x9c", "\xe2\x94\xa4", {{"Log", Style::heading}}, {}, w));
  const std::size_t log_n = h > out.size() + 3 ? h - out.size() - 3 : 0;
  std::vector<std::string> tail;
  if (j)
    for (auto it = j->log.rbegin(); it != j->log.rend() && tail.size() < log_n; ++it)
      if (!it->empty()) tail.push_back(*it);
  std::reverse(tail.begin(), tail.end());
  for (std::size_t i = 0; i < log_n; ++i) out.push_back(boxed({{i < tail.size() ? " " + tail[i] : "", Style::dim}}, w));

  out.push_back(boxed({{" " + (status_.empty() ? std::string("reading ") + draw::short_path(live_folder()) + " every 2 s" : status_), status_.empty() ? Style::dim : Style::warn}}, w));
  Spans hint{{" ", Style::plain}};
  for (const auto& [k, d] : std::vector<std::pair<const char*, const char*>>{
           {"\xe2\x86\x91\xe2\x86\x93", "job"}, {"e", "edit options"}, {"1-9", "apply advice"}, {"a", "all"}, {"r", "reload"}, {"o", "options"}, {"q", "quit"}}) {
    if (hint.size() > 1) hint.push_back({"  ", Style::plain});
    hint.push_back({k, Style::key});
    hint.push_back({std::string(" ") + d, Style::dim});
  }
  out.push_back(boxed(std::move(hint), w));
  out.push_back(border("\xe2\x94\x94", "\xe2\x94\x98", {}, {}, w));
  out.resize(h);
  return out;
}

Action run_keys(JobsModel& m, const std::string& keys) {
  for (const int k : parse_keys(keys))
    if (const Action a = m.press(k); a != Action::none) return a;
  return Action::print;
}

}  // namespace tmk::app::tui
