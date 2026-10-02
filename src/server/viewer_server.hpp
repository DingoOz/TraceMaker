#pragma once
// HTTP + WebSocket server for the live viewer (design doc 09, protocol in docs/13-viewer-protocol.md).
//
// Serves the built viewer (static files) at http://<host>:<port>/ and streams protocol messages at /ws.
// publish() is thread-safe and never blocks on the network: messages are queued and fanned out by a single
// background I/O thread. The server keeps the latest `board` snapshot plus the state changes since it, so a
// client that connects mid-run sees the current scene. Slow clients lose transient messages (frontier,
// path_try) and get coalesced stats; a client that falls hopelessly behind is disconnected (it reconnects and
// receives a fresh snapshot).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/events.hpp"

namespace tmk::server {

struct ServerOptions {
  std::string host = "0.0.0.0";
  std::uint16_t port = 8766;       // 0 = pick a free port (tests)
  std::string web_root;            // directory with index.html; empty = <source dir>/viewer/dist
  std::size_t max_queue_msgs = 2000;              // per client: beyond this, transient messages are dropped
  std::size_t max_queue_bytes = 8u << 20;         // per client: same, by bytes
  std::size_t disconnect_bytes = 256u << 20;      // per client: state backlog that drops the connection
};

class ViewerServer final : public events::Sink {
 public:
  // Binds and starts the I/O thread. Throws std::runtime_error if the address cannot be bound.
  explicit ViewerServer(ServerOptions opts = {});
  ~ViewerServer() override;
  ViewerServer(const ViewerServer&) = delete;
  ViewerServer& operator=(const ViewerServer&) = delete;

  void publish(std::string json) override;
  // True while at least one viewer is connected, so producers can skip building transient messages.
  bool wants_transient() const override;

  std::uint16_t port() const;        // bound port (useful with port 0)
  std::string url() const;           // http://<host or a LAN address>:<port>/
  std::size_t client_count() const;
  const std::string& web_root() const;

  struct Counters {
    std::uint64_t published = 0, sent = 0, dropped = 0, clients_total = 0;
  };
  Counters counters() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Default directory of the built viewer: <repo>/viewer/dist.
std::string default_web_root();

}  // namespace tmk::server
