// Probe: can Boost.Beast's TLS stack and gRPC coexist in one binary?
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <grpcpp/grpcpp.h>

#include <cstdio>

namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

int main() {
  auto creds = grpc::InsecureChannelCredentials();
  std::printf("grpc creds ok=%d\n", creds != nullptr);

  net::io_context ioc;
  ssl::context ctx(ssl::context::tlsv12_client);
  ctx.set_verify_mode(ssl::verify_none);

  tcp::resolver resolver(ioc);
  beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ctx);

  const char* host = "stream.binance.com";
  auto results = resolver.resolve(host, "9443");
  beast::get_lowest_layer(ws).connect(results);
  if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host)) {
    std::printf("SNI FAILED\n");
    return 1;
  }
  ws.next_layer().handshake(ssl::stream_base::client);
  ws.handshake(host, "/ws/btcusdt@bookTicker");
  std::printf("WSS handshake OK\n");

  beast::flat_buffer buf;
  ws.read(buf);
  std::printf("frame: %.160s\n", static_cast<const char*>(buf.data().data()));
  ws.close(beast::websocket::close_code::normal);
  return 0;
}
