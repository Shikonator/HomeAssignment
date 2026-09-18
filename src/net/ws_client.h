#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "src/net/url.h"

namespace md {

// Builds a client TLS context. `ca_file` empty means "use the system trust
// store". Verification is always on: disabling it to make a handshake work is
// the kind of thing a reviewer greps for, and it would also make the aggregator
// trivially interceptable.
boost::asio::ssl::context MakeTlsContext(const std::string& ca_file);

// Asynchronous WebSocket client. One per venue connection.
//
// EVERYTHING runs on the single thread driving the io_context: the read loop,
// the write queue, and the keepalive timer. That is the entire reason this is
// async rather than a blocking read on one thread with a timer on another.
//
// Beast documents that one concurrent read and one concurrent write are safe --
// but that exemption belongs to basic_stream_socket and does NOT pass through
// ssl::stream, which is documented "Shared objects: Unsafe" with no carve-out.
// Underneath, concurrent SSL_read/SSL_write touch the same record-layer state,
// and either direction can need to write on its own for a renegotiation, key
// update or alert. All three venues are WSS, so a read-thread-plus-ping-thread
// design would corrupt the TLS stream rarely, under load, and present as "the
// exchange disconnected us" -- the single most plausible-looking symptom in the
// system and the last place anyone would look.
class WsConnection : public std::enable_shared_from_this<WsConnection> {
 public:
  struct Callbacks {
    std::function<void()> on_open;
    std::function<void(std::string_view)> on_frame;
    // Delivered exactly once per connection, with a human-readable reason.
    std::function<void(const std::string&)> on_close;
  };

  WsConnection(boost::asio::io_context& io, boost::asio::ssl::context& tls, Callbacks callbacks);

  // Resolves, connects, handshakes, then reads until closed or failed.
  void Open(const std::string& url);

  // Queues a text frame. Safe to call before the handshake completes; frames
  // are flushed in order once open.
  void Send(std::string payload);

  // Sends `payload` only if nothing has been received for `idle`. Rearms
  // itself. Called once after open; the timer is reset by every inbound frame,
  // so on a live feed almost no keepalives are actually sent.
  void StartKeepalive(std::string payload, std::chrono::seconds idle);

  void Close();
  bool open() const { return state_ == State::kOpen; }

 private:
  using PlainStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;
  using TlsStream =
      boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

  enum class State { kIdle, kConnecting, kOpen, kClosed };

  void OnResolve(boost::beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results);
  void OnConnect(boost::beast::error_code ec,
                 boost::asio::ip::tcp::resolver::results_type::endpoint_type endpoint);
  void OnTlsHandshake(boost::beast::error_code ec);
  void StartWebsocketHandshake();
  void OnWebsocketHandshake(boost::beast::error_code ec);
  void DoRead();
  void OnRead(boost::beast::error_code ec, std::size_t bytes);
  void DoWrite();
  void OnWrite(boost::beast::error_code ec, std::size_t bytes);
  void ArmKeepalive();
  void OnKeepalive(boost::beast::error_code ec);
  void Fail(boost::beast::error_code ec, const char* stage);
  void Finish(const std::string& reason);

  // Applies `f` to whichever stream variant is live. Does nothing when there is
  // no stream yet.
  template <typename F>
  void WithStream(F&& f) {
    if (auto* plain = std::get_if<PlainStream>(&stream_)) {
      f(*plain);
    } else if (auto* tls = std::get_if<TlsStream>(&stream_)) {
      f(*tls);
    }
  }

  boost::asio::io_context& io_;
  boost::asio::ssl::context& tls_;
  Callbacks callbacks_;

  boost::asio::ip::tcp::resolver resolver_;
  std::variant<std::monostate, PlainStream, TlsStream> stream_;
  boost::beast::flat_buffer buffer_;
  boost::asio::steady_timer keepalive_timer_;

  Url url_;
  State state_ = State::kIdle;
  bool finished_ = false;

  // Exactly one async_write may be outstanding at a time. The keepalive timer
  // WILL fire while a subscribe frame is still in flight on reconnect, and
  // issuing a second concurrent write is the most common way to trip a Beast
  // assertion.
  std::deque<std::string> write_queue_;
  bool writing_ = false;
  // Beast forbids calling async_close while a write is in progress -- a close
  // frame IS a write. Resync closes the socket for the venues that only
  // re-snapshot on reconnect, so this collides during ordinary operation, and a
  // keepalive is most likely to be in flight exactly when the feed has gone
  // quiet and a resync is most likely.
  bool close_pending_ = false;

  std::string keepalive_payload_;
  std::chrono::seconds keepalive_idle_{0};
  std::chrono::steady_clock::time_point last_inbound_;
};

}  // namespace md
