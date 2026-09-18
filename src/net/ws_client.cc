#include "src/net/ws_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <utility>

namespace md {
namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

ssl::context MakeTlsContext(const std::string& ca_file) {
  ssl::context context(ssl::context::tls_client);
  context.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                      ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                      ssl::context::no_tlsv1_1);
  context.set_verify_mode(ssl::verify_peer);
  boost::system::error_code ec;
  if (ca_file.empty()) {
    context.set_default_verify_paths(ec);
  } else {
    context.load_verify_file(ca_file, ec);
  }
  // A missing trust store is surfaced by the handshake failing rather than
  // silently downgrading to no verification.
  return context;
}

WsConnection::WsConnection(net::io_context& io, ssl::context& tls, Callbacks callbacks)
    : io_(io),
      tls_(tls),
      callbacks_(std::move(callbacks)),
      resolver_(net::make_strand(io)),
      keepalive_timer_(io) {}

void WsConnection::Open(const std::string& url) {
  // Reject an http(s) URL here rather than letting it connect: the scheme is a
  // statement about protocol, and silently treating it as a websocket endpoint
  // would fail later and less clearly.
  if (!ParseUrl(url, &url_) || (url_.scheme != "ws" && url_.scheme != "wss")) {
    Finish("malformed websocket url: " + url);
    return;
  }
  state_ = State::kConnecting;
  last_inbound_ = std::chrono::steady_clock::now();

  if (url_.secure) {
    stream_.emplace<TlsStream>(net::make_strand(io_), tls_);
  } else {
    stream_.emplace<PlainStream>(net::make_strand(io_));
  }

  resolver_.async_resolve(
      url_.host, url_.port,
      [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
        self->OnResolve(ec, std::move(results));
      });
}

void WsConnection::OnResolve(beast::error_code ec, tcp::resolver::results_type results) {
  if (ec) return Fail(ec, "resolve");
  WithStream([&](auto& stream) {
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
    beast::get_lowest_layer(stream).async_connect(
        results, [self = shared_from_this()](beast::error_code connect_ec,
                                             tcp::resolver::results_type::endpoint_type endpoint) {
          self->OnConnect(connect_ec, endpoint);
        });
  });
}

void WsConnection::OnConnect(beast::error_code ec,
                             tcp::resolver::results_type::endpoint_type /*endpoint*/) {
  if (ec) return Fail(ec, "connect");

  if (auto* tls = std::get_if<TlsStream>(&stream_)) {
    // Server Name Indication. Without it the handshake is rejected by all three
    // venues, which share their IPs across many hostnames.
    if (!SSL_set_tlsext_host_name(tls->next_layer().native_handle(), url_.host.c_str())) {
      return Fail(beast::error_code{static_cast<int>(::ERR_get_error()),
                                    net::error::get_ssl_category()},
                  "sni");
    }
    tls->next_layer().set_verify_callback(ssl::host_name_verification(url_.host));
    tls->next_layer().async_handshake(
        ssl::stream_base::client,
        [self = shared_from_this()](beast::error_code handshake_ec) {
          self->OnTlsHandshake(handshake_ec);
        });
    return;
  }
  StartWebsocketHandshake();
}

void WsConnection::OnTlsHandshake(beast::error_code ec) {
  if (ec) return Fail(ec, "tls handshake");
  StartWebsocketHandshake();
}

void WsConnection::StartWebsocketHandshake() {
  WithStream([&](auto& stream) {
    // Hand timeout management to the websocket layer now that the transport is
    // up; beast::tcp_stream's own timer would otherwise fire mid-stream.
    beast::get_lowest_layer(stream).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    // The suggested client profile leaves idle_timeout unset, so a black-holed
    // connection would never fail a read. The runner also runs a silence
    // watchdog; this is the cheaper of the two and catches it first.
    timeout.idle_timeout = std::chrono::seconds(30);
    timeout.keep_alive_pings = false;  // venues need application frames, not control pings
    stream.set_option(timeout);
    // No venue sends anything remotely this large; Beast's 16MB default is an
    // unnecessary amount of trust in a remote peer.
    stream.read_message_max(4 * 1024 * 1024);
    stream.async_handshake(url_.host, url_.target,
                           [self = shared_from_this()](beast::error_code ec) {
                             self->OnWebsocketHandshake(ec);
                           });
  });
}

void WsConnection::OnWebsocketHandshake(beast::error_code ec) {
  if (ec) return Fail(ec, "websocket handshake");
  state_ = State::kOpen;
  last_inbound_ = std::chrono::steady_clock::now();
  if (callbacks_.on_open) callbacks_.on_open();
  DoRead();
  // Frames queued before the handshake completed are flushed now.
  if (!writing_ && !write_queue_.empty()) DoWrite();
}

void WsConnection::DoRead() {
  if (state_ != State::kOpen) return;
  WithStream([&](auto& stream) {
    stream.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t n) {
      self->OnRead(ec, n);
    });
  });
}

void WsConnection::OnRead(beast::error_code ec, std::size_t /*bytes*/) {
  if (ec) return Fail(ec, "read");
  last_inbound_ = std::chrono::steady_clock::now();
  if (callbacks_.on_frame) {
    const auto data = buffer_.cdata();
    callbacks_.on_frame(
        std::string_view(static_cast<const char*>(data.data()), data.size()));
  }
  buffer_.consume(buffer_.size());
  // A read is always posted. Beast only answers a peer's protocol-level ping
  // with a pong from inside a read operation, so leaving the read unposted
  // would make Binance drop us.
  DoRead();
}

void WsConnection::Send(std::string payload) {
  write_queue_.push_back(std::move(payload));
  if (state_ == State::kOpen && !writing_) DoWrite();
}

void WsConnection::DoWrite() {
  if (write_queue_.empty() || state_ != State::kOpen) return;
  writing_ = true;
  WithStream([&](auto& stream) {
    stream.text(true);
    stream.async_write(net::buffer(write_queue_.front()),
                       [self = shared_from_this()](beast::error_code ec, std::size_t n) {
                         self->OnWrite(ec, n);
                       });
  });
}

void WsConnection::OnWrite(beast::error_code ec, std::size_t /*bytes*/) {
  writing_ = false;
  if (ec) return Fail(ec, "write");
  if (!write_queue_.empty()) write_queue_.pop_front();
  if (close_pending_) {
    close_pending_ = false;
    Close();
    return;
  }
  if (!write_queue_.empty()) DoWrite();
}

void WsConnection::StartKeepalive(std::string payload, std::chrono::seconds interval) {
  if (payload.empty() || interval.count() <= 0) return;
  keepalive_payload_ = std::move(payload);
  keepalive_interval_ = interval;
  ArmKeepalive();
}

void WsConnection::ArmKeepalive() {
  if (state_ != State::kOpen && state_ != State::kConnecting) return;
  keepalive_timer_.expires_after(keepalive_interval_);
  keepalive_timer_.async_wait(
      [self = shared_from_this()](beast::error_code ec) { self->OnKeepalive(ec); });
}

void WsConnection::OnKeepalive(beast::error_code ec) {
  if (ec == net::error::operation_aborted) return;
  if (state_ != State::kOpen) {
    // Not open yet: rearm rather than silently stopping the keepalive forever.
    ArmKeepalive();
    return;
  }
  // Sent unconditionally rather than only when idle: Bybit asks for a client
  // ping every 20 seconds and appears to want it whether or not data is
  // flowing, so an idle-gated ping never fires on a busy feed and the venue
  // eventually drops us.
  Send(keepalive_payload_);
  ArmKeepalive();
}

void WsConnection::Close() {
  if (state_ != State::kOpen) {
    Finish("closed before open");
    return;
  }
  if (writing_) {
    // Defer until the in-flight write completes; OnWrite will call us back.
    close_pending_ = true;
    return;
  }
  state_ = State::kClosed;
  keepalive_timer_.cancel();
  WithStream([&](auto& stream) {
    stream.async_close(websocket::close_code::normal,
                       [self = shared_from_this()](beast::error_code) {
                         self->Finish("closed");
                       });
  });
}

void WsConnection::Fail(beast::error_code ec, const char* stage) {
  // A TLS peer that closes without completing the shutdown handshake surfaces
  // as stream_truncated. Every one of these venues does that on a normal close;
  // it is not an error and must not be reported as one.
  if (ec == ssl::error::stream_truncated || ec == websocket::error::closed) {
    Finish("peer closed");
    return;
  }
  Finish(std::string(stage) + ": " + ec.message());
}

void WsConnection::Finish(const std::string& reason) {
  // Delivered exactly once: the read, write and timer handlers can all fail
  // from the same underlying error, and the runner must not reconnect N times
  // for one disconnect.
  if (finished_) return;
  finished_ = true;
  state_ = State::kClosed;
  keepalive_timer_.cancel();
  if (callbacks_.on_close) callbacks_.on_close(reason);
}

}  // namespace md
