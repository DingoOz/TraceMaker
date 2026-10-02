#include "server/viewer_server.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "server/messages.hpp"

#ifndef TM_SOURCE_DIR
#define TM_SOURCE_DIR "."
#endif

namespace tmk::server {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace fs = std::filesystem;
using tcp = net::ip::tcp;
using Text = std::shared_ptr<const std::string>;

// How the server treats a message (docs/13-viewer-protocol.md: transient messages may be dropped).
enum class Kind : std::uint8_t {
  Board,      // full snapshot: supersedes queued scene deltas
  Delta,      // scene change (tracks, vias, footprints): must be delivered
  Info,       // stage, log, failure, ratsnest, heatmap: must be delivered
  Stats,      // progress counters: only the latest matters
  Transient,  // frontier, path_try: droppable
};

Kind classify(std::string_view type) {
  if (type == "board") return Kind::Board;
  if (type == "stats") return Kind::Stats;
  if (type == "frontier" || type == "path_try") return Kind::Transient;
  if (type == "track_add" || type == "track_remove" || type == "via_add" || type == "via_remove" ||
      type == "footprint_move")
    return Kind::Delta;
  return Kind::Info;
}

struct Msg {
  Text text;
  Kind kind = Kind::Info;
};

class Hub;

// ---------------------------------------------------------------------------------------------------------
// One WebSocket client. All members are touched only from the server's single I/O thread.
class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket&& socket, Hub& hub) : ws_(std::move(socket)), hub_(hub) {}

  void start(http::request<http::string_body> req);
  void enqueue(const Msg& m);
  void close();
  bool open() const { return open_; }

 private:
  void on_accept(beast::error_code ec);
  void do_read();
  void do_write();
  void fail();
  bool over_bound() const;
  void purge(bool drop_stats, bool drop_deltas);

  websocket::stream<beast::tcp_stream> ws_;
  Hub& hub_;
  beast::flat_buffer rbuf_;
  std::deque<Msg> queue_;
  std::size_t queued_bytes_ = 0;
  bool writing_ = false;
  bool open_ = false;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------------------------------------
// Server state: acceptor, sessions and the scene state needed to bring a late client up to date. One I/O
// thread runs every handler, so the single-threaded io_context is the strand for all of this state.
class Hub {
 public:
  Hub(ServerOptions opts, std::string root) : opts_(std::move(opts)), root_(std::move(root)) {}

  ServerOptions opts_;
  std::string root_;
  net::io_context ioc_{1};
  tcp::acceptor acceptor_{ioc_};
  std::set<std::shared_ptr<WsSession>> sessions_;

  // Inbox filled by publish() from any thread.
  std::mutex mu_;
  std::vector<std::string> inbox_;
  bool drain_posted_ = false;

  std::atomic<std::size_t> clients_{0};
  std::atomic<std::uint64_t> published_{0}, sent_{0}, dropped_{0}, clients_total_{0};

  // Scene state since the last board snapshot (I/O thread only).
  Text board_;
  std::map<ObjectId, Text> track_adds_, via_adds_;
  std::set<ObjectId> track_removed_, via_removed_;  // snapshot ids removed since
  std::map<std::string, Text> fp_moves_, heatmaps_;
  Text ratsnest_, stats_;
  std::deque<Text> history_;  // recent stage / log / failure messages
  static constexpr std::size_t kHistory = 300;

  void accept() {
    acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket s) {
      if (ec) {
        if (ec == net::error::operation_aborted || !acceptor_.is_open()) return;
      } else {
        start_http(std::move(s));
      }
      accept();
    });
  }

  void start_http(tcp::socket&& s);

  void post_message(std::string json) {
    bool post = false;
    {
      std::lock_guard lock(mu_);
      // Safety valve if the I/O thread cannot keep up at all: drop transient messages at the door.
      if (inbox_.size() > 100'000 && classify(message_type(json)) == Kind::Transient) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      inbox_.push_back(std::move(json));
      if (!drain_posted_) drain_posted_ = post = true;
    }
    if (post) net::post(ioc_, [this] { drain(); });
  }

  void drain() {
    std::vector<std::string> batch;
    {
      std::lock_guard lock(mu_);
      batch.swap(inbox_);
      drain_posted_ = false;
    }
    for (auto& m : batch) dispatch(std::move(m));
  }

  static std::optional<ObjectId> id_of(const std::string& json, const char* obj) {
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    const nlohmann::json* p = &j;
    if (obj) {
      auto it = j.find(obj);
      if (it == j.end()) return std::nullopt;
      p = &*it;
    }
    auto id = p->find("id");
    if (id == p->end() || !id->is_number_integer()) return std::nullopt;
    return id->get<ObjectId>();
  }

  void remember(const std::string& type, Kind kind, const Text& t) {
    switch (kind) {
      case Kind::Board:
        board_ = t;
        track_adds_.clear();
        via_adds_.clear();
        track_removed_.clear();
        via_removed_.clear();
        fp_moves_.clear();
        return;
      case Kind::Stats: stats_ = t; return;
      case Kind::Transient: return;
      case Kind::Delta:
      case Kind::Info: break;
    }
    auto add = [&](std::map<ObjectId, Text>& adds, const char* obj) {
      if (auto id = id_of(*t, obj)) adds[*id] = t;
    };
    auto remove = [&](std::map<ObjectId, Text>& adds, std::set<ObjectId>& removed) {
      if (auto id = id_of(*t, nullptr)) {
        if (adds.erase(*id) == 0) removed.insert(*id);
      }
    };
    if (type == "track_add") add(track_adds_, "track");
    else if (type == "via_add") add(via_adds_, "via");
    else if (type == "track_remove") remove(track_adds_, track_removed_);
    else if (type == "via_remove") remove(via_adds_, via_removed_);
    else if (type == "ratsnest") ratsnest_ = t;
    else if (type == "footprint_move" || type == "heatmap") {
      const auto j = nlohmann::json::parse(*t, nullptr, false);
      if (!j.is_object()) return;
      if (type == "footprint_move") {
        fp_moves_[j.value("ref", std::string())] = t;
      } else {
        heatmaps_[j.value("name", std::string()) + "@" + std::to_string(j.value("layer", -1))] = t;
      }
    } else {
      history_.push_back(t);
      if (history_.size() > kHistory) history_.pop_front();
    }
  }

  void dispatch(std::string json) {
    published_.fetch_add(1, std::memory_order_relaxed);
    const std::string type = message_type(json);
    const Kind kind = classify(type);
    auto t = std::make_shared<const std::string>(std::move(json));
    remember(type, kind, t);
    for (const auto& s : sessions_)
      if (s->open()) s->enqueue({t, kind});
  }

  // Brings a new client up to date: snapshot, the deltas since, then the latest of everything else.
  void on_open(const std::shared_ptr<WsSession>& s) {
    sessions_.insert(s);
    clients_.fetch_add(1);
    clients_total_.fetch_add(1);
    if (board_) {
      s->enqueue({board_, Kind::Board});
      for (ObjectId id : track_removed_) s->enqueue({std::make_shared<const std::string>(track_remove(id)), Kind::Delta});
      for (const auto& [id, t] : track_adds_) s->enqueue({t, Kind::Delta});
      for (ObjectId id : via_removed_) s->enqueue({std::make_shared<const std::string>(via_remove(id)), Kind::Delta});
      for (const auto& [id, t] : via_adds_) s->enqueue({t, Kind::Delta});
      for (const auto& [ref, t] : fp_moves_) s->enqueue({t, Kind::Delta});
    }
    if (ratsnest_) s->enqueue({ratsnest_, Kind::Info});
    for (const auto& [k, t] : heatmaps_) s->enqueue({t, Kind::Info});
    for (const auto& t : history_) s->enqueue({t, Kind::Info});
    if (stats_) s->enqueue({stats_, Kind::Stats});
  }

  void remove(const std::shared_ptr<WsSession>& s) {
    if (sessions_.erase(s) > 0) clients_.fetch_sub(1);
  }
};

// ---------------------------------------------------------------------------------------------------------
void WsSession::start(http::request<http::string_body> req) {
  ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) { res.set(http::field::server, "tracemaker-view"); }));
  ws_.read_message_max(1 << 20);
  ws_.async_accept(req, [self = shared_from_this()](beast::error_code ec) { self->on_accept(ec); });
}

void WsSession::on_accept(beast::error_code ec) {
  if (ec || closed_) return;
  open_ = true;
  ws_.text(true);
  hub_.on_open(shared_from_this());
  do_read();
}

// Incoming messages are reserved for the control channel (doc 09 §2); v1 reads and ignores them so that
// close and ping frames are processed.
void WsSession::do_read() {
  ws_.async_read(rbuf_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
    if (ec) {
      self->fail();
      return;
    }
    self->rbuf_.clear();
    self->do_read();
  });
}

bool WsSession::over_bound() const {
  return queue_.size() > hub_.opts_.max_queue_msgs || queued_bytes_ > hub_.opts_.max_queue_bytes;
}

// Removes queued (not in-flight) messages: always transient ones, optionally stats and scene deltas.
void WsSession::purge(bool drop_stats, bool drop_deltas) {
  const std::size_t first = writing_ ? 1 : 0;
  std::deque<Msg> keep;
  for (std::size_t i = 0; i < queue_.size(); ++i) {
    const Msg& m = queue_[i];
    const bool drop = i >= first && (m.kind == Kind::Transient || (drop_stats && m.kind == Kind::Stats) ||
                                     (drop_deltas && m.kind == Kind::Delta));
    if (drop) {
      queued_bytes_ -= m.text->size();
      hub_.dropped_.fetch_add(1, std::memory_order_relaxed);
    } else {
      keep.push_back(m);
    }
  }
  queue_.swap(keep);
}

void WsSession::enqueue(const Msg& m) {
  if (closed_) return;
  if (m.kind == Kind::Board) purge(false, true);  // a snapshot supersedes queued deltas
  if (over_bound()) {
    if (m.kind == Kind::Transient) {
      hub_.dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    purge(m.kind == Kind::Stats, false);
  }
  queue_.push_back(m);
  queued_bytes_ += m.text->size();
  if (queued_bytes_ > hub_.opts_.disconnect_bytes) {
    // Hopelessly behind on state: drop the connection; the client reconnects and gets a fresh snapshot.
    fail();
    return;
  }
  if (!writing_) do_write();
}

void WsSession::do_write() {
  writing_ = true;
  ws_.async_write(net::buffer(*queue_.front().text), [self = shared_from_this()](beast::error_code ec, std::size_t) {
    if (ec || self->closed_) {
      self->fail();
      return;
    }
    self->hub_.sent_.fetch_add(1, std::memory_order_relaxed);
    self->queued_bytes_ -= self->queue_.front().text->size();
    self->queue_.pop_front();
    if (self->queue_.empty()) {
      self->writing_ = false;
      return;
    }
    self->do_write();
  });
}

void WsSession::close() {
  if (closed_) return;
  closed_ = true;
  open_ = false;
  beast::error_code ec;
  beast::get_lowest_layer(ws_).socket().shutdown(tcp::socket::shutdown_both, ec);
  beast::get_lowest_layer(ws_).socket().close(ec);
}

void WsSession::fail() {
  if (closed_) return;
  close();
  queue_.clear();
  queued_bytes_ = 0;
  // Deferred so that a session never leaves the hub's set while the hub is iterating over it.
  net::post(hub_.ioc_, [self = shared_from_this()] { self->hub_.remove(self); });
}

// ---------------------------------------------------------------------------------------------------------
std::string_view mime_type(std::string_view path) {
  const auto dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string ext(path.substr(dot + 1));
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::map<std::string, std::string_view, std::less<>> kTypes = {
      {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
      {"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"},
      {"css", "text/css; charset=utf-8"}, {"json", "application/json"}, {"map", "application/json"},
      {"svg", "image/svg+xml"}, {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
      {"gif", "image/gif"}, {"webp", "image/webp"}, {"ico", "image/x-icon"}, {"wasm", "application/wasm"},
      {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"}, {"txt", "text/plain; charset=utf-8"},
      {"webmanifest", "application/manifest+json"}};
  auto it = kTypes.find(ext);
  return it == kTypes.end() ? std::string_view("application/octet-stream") : it->second;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Maps a request target to a file under root, or nullopt if the path is malformed or escapes the root.
std::optional<fs::path> resolve_target(const std::string& root, std::string_view target) {
  if (auto q = target.find_first_of("?#"); q != std::string_view::npos) target = target.substr(0, q);
  if (target.empty() || target.front() != '/') return std::nullopt;
  std::string decoded;
  for (std::size_t i = 0; i < target.size(); ++i) {
    if (target[i] == '%') {
      if (i + 2 >= target.size()) return std::nullopt;
      const int hi = hex_value(target[i + 1]), lo = hex_value(target[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      decoded.push_back(static_cast<char>(hi * 16 + lo));
      i += 2;
    } else {
      decoded.push_back(target[i]);
    }
  }
  if (decoded.find('\0') != std::string::npos || decoded.find('\\') != std::string::npos) return std::nullopt;
  if (decoded.back() == '/') decoded += "index.html";
  fs::path rel;
  std::size_t pos = 1;
  while (pos <= decoded.size()) {
    const std::size_t next = std::min(decoded.find('/', pos), decoded.size());
    const std::string seg = decoded.substr(pos, next - pos);
    if (seg == "..") return std::nullopt;
    if (!seg.empty() && seg != ".") rel /= seg;
    pos = next + 1;
  }
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(root, ec);
  if (ec) return std::nullopt;
  const fs::path full = fs::weakly_canonical(base / rel, ec);
  if (ec) return std::nullopt;
  // Symlinks inside the root must not lead outside it either.
  const auto b = base.native(), f = full.native();
  if (f.size() < b.size() || f.compare(0, b.size(), b) != 0 || (f.size() > b.size() && f[b.size()] != '/'))
    return std::nullopt;
  return full;
}

constexpr std::string_view kNotBuiltPage = R"(<!doctype html><html><head><meta charset="utf-8"><title>TraceMaker</title>
<style>body{background:#0b1020;color:#cfd8ea;font:15px/1.5 system-ui,sans-serif;display:grid;place-items:center;height:100vh;margin:0}
code{background:#151c33;padding:2px 6px;border-radius:4px}</style></head><body><div>
<h2>TraceMaker viewer is not built</h2><p>Run <code>cd viewer &amp;&amp; npm install &amp;&amp; npm run build</code>,
or pass <code>--web DIR</code>. The event stream is live at <code>/ws</code>.</p></div></body></html>)";

template <class Body>
http::message_generator handle_request(const std::string& root, http::request<Body>&& req) {
  auto text = [&](http::status st, std::string_view type, std::string body) {
    http::response<http::string_body> res{st, req.version()};
    res.set(http::field::server, "tracemaker-view");
    res.set(http::field::content_type, type);
    res.keep_alive(req.keep_alive());
    res.body() = std::move(body);
    res.prepare_payload();
    return http::message_generator(std::move(res));
  };
  if (req.method() != http::verb::get && req.method() != http::verb::head)
    return text(http::status::method_not_allowed, "text/plain", "method not allowed\n");
  const auto path = resolve_target(root, std::string_view(req.target().data(), req.target().size()));
  if (!path) return text(http::status::bad_request, "text/plain", "bad path\n");

  beast::error_code ec;
  http::file_body::value_type body;
  body.open(path->c_str(), beast::file_mode::scan, ec);
  std::error_code fec;
  if (ec || !fs::is_regular_file(*path, fec)) {
    const std::string t(req.target().data(), req.target().size());
    if ((t == "/" || t.starts_with("/index.html") || t.starts_with("/?")) && !fs::exists(fs::path(root) / "index.html", fec))
      return text(http::status::ok, "text/html; charset=utf-8", std::string(kNotBuiltPage));
    return text(http::status::not_found, "text/plain", "not found\n");
  }
  const auto size = body.size();
  const auto mime = mime_type(path->native());
  // Vite puts content-hashed files under assets/: cache those; revalidate everything else.
  const bool immutable = path->parent_path().filename() == "assets";
  if (req.method() == http::verb::head) {
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "tracemaker-view");
    res.set(http::field::content_type, mime);
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return res;
  }
  http::response<http::file_body> res{std::piecewise_construct, std::make_tuple(std::move(body)),
                                      std::make_tuple(http::status::ok, req.version())};
  res.set(http::field::server, "tracemaker-view");
  res.set(http::field::content_type, mime);
  res.set(http::field::cache_control, immutable ? "public, max-age=31536000, immutable" : "no-cache");
  res.content_length(size);
  res.keep_alive(req.keep_alive());
  return res;
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket&& s, Hub& hub) : stream_(std::move(s)), hub_(hub) {}

  void run() {
    net::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->do_read(); });
  }

 private:
  void do_read() {
    parser_.emplace();
    parser_->body_limit(64 * 1024);
    stream_.expires_after(std::chrono::seconds(60));
    http::async_read(stream_, buf_, *parser_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
      self->on_read(ec);
    });
  }

  void on_read(beast::error_code ec) {
    if (ec == http::error::end_of_stream) {
      stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
      return;
    }
    if (ec) return;
    auto req = parser_->release();
    const std::string_view target(req.target().data(), req.target().size());
    if (websocket::is_upgrade(req)) {
      if (target == "/ws" || target.starts_with("/ws?")) {
        stream_.expires_never();  // the websocket stream manages its own timeouts
        std::make_shared<WsSession>(stream_.release_socket(), hub_)->start(std::move(req));
      }
      return;
    }
    const bool keep_alive = req.keep_alive();
    beast::async_write(stream_, handle_request(hub_.root_, std::move(req)),
                       [self = shared_from_this(), keep_alive](beast::error_code wec, std::size_t) {
                         if (wec) return;
                         if (!keep_alive) {
                           beast::error_code sec;
                           self->stream_.socket().shutdown(tcp::socket::shutdown_send, sec);
                           return;
                         }
                         self->do_read();
                       });
  }

  beast::tcp_stream stream_;
  Hub& hub_;
  beast::flat_buffer buf_;
  std::optional<http::request_parser<http::string_body>> parser_;
};

void Hub::start_http(tcp::socket&& s) { std::make_shared<HttpSession>(std::move(s), *this)->run(); }

// Address other machines on the LAN can use to reach this host (no packet is sent by a UDP connect).
std::string lan_address() {
  try {
    net::io_context ioc;
    net::ip::udp::socket s(ioc);
    s.connect({net::ip::make_address("192.0.2.1"), 9});
    return s.local_endpoint().address().to_string();
  } catch (const std::exception&) {
    return "127.0.0.1";
  }
}

}  // namespace

std::string default_web_root() { return (fs::path(TM_SOURCE_DIR) / "viewer" / "dist").string(); }

struct ViewerServer::Impl {
  Hub hub;
  std::thread thread;
  std::uint16_t port = 0;
  explicit Impl(ServerOptions o) : hub(o, o.web_root.empty() ? default_web_root() : o.web_root) {}
};

ViewerServer::ViewerServer(ServerOptions opts) : impl_(std::make_unique<Impl>(std::move(opts))) {
  Hub& h = impl_->hub;
  beast::error_code ec;
  const auto addr = net::ip::make_address(h.opts_.host, ec);
  if (ec) throw std::runtime_error("viewer server: bad address '" + h.opts_.host + "'");
  const tcp::endpoint ep{addr, h.opts_.port};
  h.acceptor_.open(ep.protocol(), ec);
  if (!ec) h.acceptor_.set_option(net::socket_base::reuse_address(true), ec);
  if (!ec) h.acceptor_.bind(ep, ec);
  if (!ec) h.acceptor_.listen(net::socket_base::max_listen_connections, ec);
  if (ec)
    throw std::runtime_error("viewer server: cannot listen on " + h.opts_.host + ":" + std::to_string(h.opts_.port) +
                             ": " + ec.message());
  impl_->port = h.acceptor_.local_endpoint().port();
  h.accept();
  impl_->thread = std::thread([&h] {
    auto guard = net::make_work_guard(h.ioc_);
    h.ioc_.run();
  });
}

ViewerServer::~ViewerServer() {
  Hub& h = impl_->hub;
  // Stop accepting and close every connection on the I/O thread, then stop it.
  std::promise<void> done;
  auto fut = done.get_future();
  net::post(h.ioc_, [&h, &done] {
    beast::error_code ec;
    h.acceptor_.close(ec);
    for (const auto& s : h.sessions_) s->close();
    done.set_value();
  });
  fut.wait_for(std::chrono::seconds(2));
  h.ioc_.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
  h.sessions_.clear();
  h.clients_ = 0;
}

void ViewerServer::publish(std::string json) { impl_->hub.post_message(std::move(json)); }

bool ViewerServer::wants_transient() const { return impl_->hub.clients_.load(std::memory_order_relaxed) > 0; }

std::uint16_t ViewerServer::port() const { return impl_->port; }

std::string ViewerServer::url() const {
  const std::string& host = impl_->hub.opts_.host;
  const std::string shown = (host == "0.0.0.0" || host == "::") ? lan_address() : host;
  return "http://" + shown + ":" + std::to_string(impl_->port) + "/";
}

std::size_t ViewerServer::client_count() const { return impl_->hub.clients_.load(); }

const std::string& ViewerServer::web_root() const { return impl_->hub.root_; }

ViewerServer::Counters ViewerServer::counters() const {
  const Hub& h = impl_->hub;
  return {h.published_.load(), h.sent_.load(), h.dropped_.load(), h.clients_total_.load()};
}

}  // namespace tmk::server
