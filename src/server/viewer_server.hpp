// SPDX-License-Identifier: GPL-3.0-or-later
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

// Who can reach the viewer (issue #4, docs/09-visualization.md "Security", D116): by default only this machine. A viewer bound to any other address
// requires a token on the page and on /ws (a random one unless `token` is given; `allow_open` serves without, a deliberate choice that is said so),
// and every WebSocket upgrade must come from the server's own origin, so a web page open in the user's browser cannot read the stream.
// The bind address when none is given: $TM_VIEW_HOST if set, else 127.0.0.1 (this machine only).
std::string default_host();

struct ServerOptions {
  std::string host = default_host();  // "0.0.0.0" or a LAN address makes the viewer reachable from the network: it then needs a token
  std::uint16_t port = 8766;       // 0 = pick a free port (tests)
  std::string token;               // at least 16 characters of [A-Za-z0-9._~-]; empty = generated when the host is not a loopback address
  bool allow_open = false;         // a non-loopback host without any token (--view-insecure)
  std::string embed;               // pages that may frame the viewer, as CSP sources separated by spaces (--view-embed); empty = nobody (X-Frame-Options: DENY)
  std::size_t max_clients = 16;        // WebSocket viewers, counted from the upgrade on (a burst cannot overshoot); the next gets 503
  std::size_t max_connections = 128;   // concurrent HTTP connections (a page load is one); beyond it a connection is closed at once
  std::size_t max_connections_per_address = 16;  // of those, from one address: one host cannot use them all up before it has authenticated
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
  std::string url() const;           // http://<host or a LAN address>:<port>/ , with ?token=... when the viewer needs one
  const std::string& token() const;  // "" = none required
  bool exposed() const;              // bound to an address other machines can reach
  // What to tell the user when the viewer is reachable from the network ("" when it is not): who can see it and what protects it.
  std::string notice() const;
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

// Default directory of the built viewer: $TM_VIEW_DIR, else <prefix>/share/tracemaker/viewer for a binary in <prefix>/bin, else <repo>/viewer/dist.
std::string default_web_root();

}  // namespace tmk::server
