#pragma once
// Persistent knowledge base (design doc 06 §4, failure-memory tier T3): what was learned on earlier runs.
//
//  * per board-feature bucket: how often each router variant was run and how often it produced the best result
//    (Thompson-sampling bandit over portfolio variants);
//  * per board (content hash): connections that failed before, so a re-run routes them first.
//
// SQLite file, default ~/.local/share/tracemaker/kb.sqlite. Every call is best-effort: a missing or locked
// database never stops routing.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model/board.hpp"

namespace tmk::learn {

struct BoardFeatures {
  std::uint64_t hash = 0;   // content hash of the board file
  int layers = 0, nets = 0, pads = 0, connections = 0;
  double area_cm2 = 0;
  std::string bucket() const;  // coarse class used to share statistics between similar boards
};

BoardFeatures features_of(const model::Board& b, const std::string& file_text, int connections);

struct FailedConnection {
  std::string net, pad_a, pad_b;  // "REF.NUM"
  int fails = 0;
};

class KnowledgeBase {
 public:
  explicit KnowledgeBase(std::string path = default_path());
  ~KnowledgeBase();
  bool ok() const;
  static std::string default_path();

  // Bandit: returns `k` variant indices out of `n`, the historically best first, the rest by Thompson sampling
  // on Beta(1 + wins, 1 + plays - wins) for this feature bucket (seeded, deterministic).
  std::vector<int> choose_variants(const BoardFeatures& f, int n, int k, std::uint64_t seed) const;
  void record_run(const BoardFeatures& f, const std::string& board_name, const std::vector<int>& variants_run, int best_variant,
                  int routed, int connections, double seconds);

  std::vector<FailedConnection> failed_connections(std::uint64_t board_hash) const;
  void record_failures(std::uint64_t board_hash, const std::vector<FailedConnection>& failed);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tmk::learn
