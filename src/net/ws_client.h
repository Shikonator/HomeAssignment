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

// Empty `ca_file` uses the system trust store. Verification is always on.
boost::asio::ssl::context MakeTlsContext(const std::string& ca_file);

// Asynchronous WebSocket client, one per venue connection.
//
// EVERYTHING runs on the one thread driving the io_context, and that is the
// whole reason this is async. Beast's "one concurrent read and one concurrent
// write is safe" belongs to basic_stream_socket and does NOT pass through
// ssl::stream. All three venues are WSS, so a read-thread-plus-ping-thread
// design would corrupt TLS under load and present as a venue disconnect.
class WsConnection : public std::enable_shared_from_this<WsConnection> {
 public:
  struct Callbacks {
    std::function<void()> on_open;
    std::function<void(std::string_view)> on_frame;
    // Delivered exactly once per connection.
    std::function<void(const std::string&)> on_close;
  };

  WsConnection(boost::asio::io_context& io, boost::asio::ssl::context& tls, Callbacks callbacks);

  void Open(const std::string& url);

  // Safe before the handshake; queued frames flush in order once open.
  void Send(std::string payload);

  // Unconditional, not idle-gated. See VenueProtocol::keepalive_interval.
  void StartKeepalive(std::string payload, std::chrono::seconds interval);

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

  // Applies `f` to whichever stream variant is live.
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
  std::chrono::seconds keepalive_interval_{0};
  std::chrono::steady_clock::time_point last_inbound_;
};

}  // namespace md
