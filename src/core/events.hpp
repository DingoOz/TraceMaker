#pragma once
// Event sink for live visualisation and replay (design doc 02 §7, protocol in docs/13-viewer-protocol.md).
// Producers (router, placer, DRC) publish JSON messages; the viewer server implements Sink. Publishing must
// never block a stage: implementations queue and coalesce.
#include <memory>
#include <string>

namespace tmk::events {

class Sink {
 public:
  virtual ~Sink() = default;
  // One JSON object per message (see the protocol document for the message types).
  virtual void publish(std::string json) = 0;
  // Transient, droppable messages (search frontiers, ghosts). Return false to let producers skip building them.
  virtual bool wants_transient() const { return true; }
};

class NullSink final : public Sink {
 public:
  void publish(std::string) override {}
  bool wants_transient() const override { return false; }
};

}  // namespace tmk::events
