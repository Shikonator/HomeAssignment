#include "src/net/http_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <memory>
#include <utility>

namespace md {
namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

struct Url {
  bool secure = true;
  std::string host;
  std::string port;
  std::string target = "/";
};

bool ParseHttpUrl(std::string_view url, Url* out) {
  if (url.starts_with("https://")) {
    out->secure = true;
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    out->secure = false;
    url.remove_prefix(7);
  } else {
    return false;
  }
  const std::size_t slash = url.find('/');
  std::string_view authority = url.substr(0, slash);
  out->target = (slash == std::string_view::npos) ? "/" : std::string(url.substr(slash));
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    out->host = std::string(authority.substr(0, colon));
    out->port = std::string(authority.substr(colon + 1));
  } else {
    out->host = std::string(authority);
    out->port = out->secure ? "443" : "80";
  }
  return !out->host.empty();
}

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(net::io_context& io, ssl::context& tls, Url url, HttpGetHandler handler)
      : resolver_(net::make_strand(io)),
        tls_stream_(net::make_strand(io), tls),
        plain_stream_(net::make_strand(io)),
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

  void OnResolve(tcp::resolver::results_type results) {
    if (url_.secure) {
      beast::get_lowest_layer(tls_stream_).expires_after(std::chrono::seconds(15));
      beast::get_lowest_layer(tls_stream_)
          .async_connect(results, [self = shared_from_this()](
                                      beast::error_code ec,
                                      tcp::resolver::results_type::endpoint_type) {
            if (ec) return self->Finish(false, {}, "connect: " + ec.message());
            self->OnTlsConnect();
          });
      return;
    }
    beast::get_lowest_layer(plain_stream_).expires_after(std::chrono::seconds(15));
    plain_stream_.async_connect(
        results, [self = shared_from_this()](beast::error_code ec,
                                             tcp::resolver::results_type::endpoint_type) {
          if (ec) return self->Finish(false, {}, "connect: " + ec.message());
          http::async_write(self->plain_stream_, self->request_,
                            [self](beast::error_code write_ec, std::size_t) {
                              if (write_ec) {
                                return self->Finish(false, {}, "write: " + write_ec.message());
                              }
                              self->Read(self->plain_stream_);
                            });
        });
  }

  void OnTlsConnect() {
    if (!SSL_set_tlsext_host_name(tls_stream_.native_handle(), url_.host.c_str())) {
      return Finish(false, {}, "sni failed");
    }
    tls_stream_.set_verify_callback(ssl::host_name_verification(url_.host));
    tls_stream_.async_handshake(ssl::stream_base::client,
                                [self = shared_from_this()](beast::error_code ec) {
                                  if (ec) {
                                    return self->Finish(false, {}, "tls: " + ec.message());
                                  }
                                  http::async_write(
                                      self->tls_stream_, self->request_,
                                      [self](beast::error_code write_ec, std::size_t) {
                                        if (write_ec) {
                                          return self->Finish(false, {},
                                                              "write: " + write_ec.message());
                                        }
                                        self->Read(self->tls_stream_);
                                      });
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
  if (!ParseHttpUrl(url, &parsed)) {
    handler(false, {}, "malformed url: " + url);
    return;
  }
  std::make_shared<Session>(io, tls, std::move(parsed), std::move(handler))->Run();
}

}  // namespace md
