#include "src/net/http_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include "src/net/url.h"

#include <memory>
#include <utility>

namespace md {
namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(net::io_context& io, ssl::context& tls, Url url, HttpGetHandler handler)
      : strand_(net::make_strand(io)),
        resolver_(strand_),
        tls_stream_(strand_, tls),
        plain_stream_(strand_),
        url_(std::move(url)),
        handler_(std::move(handler)) {}

  void Run() {
    request_.version(11);
    request_.method(http::verb::get);
    request_.target(url_.target);
    request_.set(http::field::host, url_.host);
    request_.set(http::field::user_agent, "cex-aggregator/1.0");

    resolver_.async_resolve(url_.host, url_.port,
                            [self = shared_from_this()](beast::error_code ec,
                                                        tcp::resolver::results_type results) {
                              if (ec) return self->Finish(false, {}, "resolve: " + ec.message());
                              self->OnResolve(std::move(results));
                            });
  }

 private:
  template <typename Stream>
  void Read(Stream& stream) {
    http::async_read(stream, buffer_, response_,
                     [self = shared_from_this(), &stream](beast::error_code ec, std::size_t) {
                       if (ec) return self->Finish(false, {}, "read: " + ec.message());
                       self->OnResponse();
                     });
  }

  // Both schemes follow the same five steps -- resolve, connect, (handshake),
  // write, read -- so both are written as named steps rather than one path
  // using members and the other nesting lambdas three deep.
  void OnResolve(tcp::resolver::results_type results) {
    auto& lowest = url_.secure ? beast::get_lowest_layer(tls_stream_)
                               : beast::get_lowest_layer(plain_stream_);
    lowest.expires_after(std::chrono::seconds(15));
    lowest.async_connect(results,
                         [self = shared_from_this()](
                             beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
                           if (ec) return self->Finish(false, {}, "connect: " + ec.message());
                           self->OnConnect();
                         });
  }

  void OnConnect() {
    if (url_.secure) return OnTlsConnect();
    Write(plain_stream_);
  }

  template <typename Stream>
  void Write(Stream& stream) {
    http::async_write(stream, request_,
                      [self = shared_from_this(), &stream](beast::error_code ec, std::size_t) {
                        if (ec) return self->Finish(false, {}, "write: " + ec.message());
                        self->Read(stream);
                      });
  }

  void OnTlsConnect() {
    if (!SSL_set_tlsext_host_name(tls_stream_.native_handle(), url_.host.c_str())) {
      return Finish(false, {}, "sni failed");
    }
    tls_stream_.set_verify_callback(ssl::host_name_verification(url_.host));
    tls_stream_.async_handshake(ssl::stream_base::client,
                                [self = shared_from_this()](beast::error_code ec) {
                                  if (ec) return self->Finish(false, {}, "tls: " + ec.message());
                                  self->Write(self->tls_stream_);
                                });
  }

  void OnResponse() {
    const unsigned status = response_.result_int();
    if (status != 200) {
      // Surfaced rather than swallowed: a 418 or 429 from Binance means we are
      // being rate limited, which the runner must back off from rather than
      // retry immediately.
      Finish(false, {}, "http status " + std::to_string(status));
      return;
    }
    Finish(true, std::move(response_.body()), {});
  }

  void Finish(bool ok, std::string body, std::string error) {
    if (done_) return;
    done_ = true;
    beast::error_code ignored;
    beast::get_lowest_layer(tls_stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
    plain_stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
    handler_(ok, std::move(body), std::move(error));
  }

  // ONE strand shared by every object in this session. Three separate strands
  // would serialise nothing with respect to each other, which is the version
  // that looks safe and is not -- Finish() touches both stream members and must
  // not race the handler that invoked it. Today the runner drives one
  // io_context per venue on a single thread, so this is belt and braces; it
  // stops being belt and braces the day someone uses a thread pool.
  net::strand<net::io_context::executor_type> strand_;
  tcp::resolver resolver_;
  beast::ssl_stream<beast::tcp_stream> tls_stream_;
  beast::tcp_stream plain_stream_;
  Url url_;
  HttpGetHandler handler_;
  http::request<http::empty_body> request_;
  http::response<http::string_body> response_;
  beast::flat_buffer buffer_;
  bool done_ = false;
};

}  // namespace

void HttpGet(net::io_context& io, ssl::context& tls, const std::string& url,
             HttpGetHandler handler) {
  Url parsed;
  if (!ParseUrl(url, &parsed) || (parsed.scheme != "http" && parsed.scheme != "https")) {
    handler(false, {}, "malformed url: " + url);
    return;
  }
  std::make_shared<Session>(io, tls, std::move(parsed), std::move(handler))->Run();
}

}  // namespace md
