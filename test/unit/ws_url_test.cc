#include <gtest/gtest.h>

#include "src/net/ws_client.h"

namespace md {
namespace {

WsUrl Parse(const char* url) {
  WsUrl parsed;
  EXPECT_TRUE(ParseWsUrl(url, &parsed)) << url;
  return parsed;
}

TEST(ParseWsUrl, ExplicitPort) {
  const WsUrl url = Parse("wss://stream.binance.com:9443/ws/btcusdt@depth@100ms");
  EXPECT_TRUE(url.secure);
  EXPECT_EQ(url.host, "stream.binance.com");
  EXPECT_EQ(url.port, "9443");
  EXPECT_EQ(url.target, "/ws/btcusdt@depth@100ms");
}

TEST(ParseWsUrl, PortDefaultsByScheme) {
  const WsUrl secure = Parse("wss://stream.bybit.com/v5/public/spot");
  EXPECT_EQ(secure.port, "443");
  const WsUrl plain = Parse("ws://127.0.0.1/mock");
  EXPECT_FALSE(plain.secure);
  EXPECT_EQ(plain.port, "80");
}

TEST(ParseWsUrl, PlainSchemeWithPort) {
  const WsUrl url = Parse("ws://127.0.0.1:8080/mock");
  EXPECT_FALSE(url.secure);
  EXPECT_EQ(url.host, "127.0.0.1");
  EXPECT_EQ(url.port, "8080");
}

TEST(ParseWsUrl, TargetDefaultsToRoot) {
  const WsUrl url = Parse("wss://ws.okx.com:8443");
  EXPECT_EQ(url.target, "/");
}

// REGRESSION. The port separator is the last colon AFTER the closing bracket,
// not simply "the last colon unless brackets appear anywhere" -- that rule made
// any bracketed authority unsplittable, so an explicit port was swallowed into
// the host and the connection silently fell back to 443.
TEST(ParseWsUrl, IPv6LiteralWithExplicitPort) {
  const WsUrl url = Parse("wss://[::1]:8443/ws");
  EXPECT_EQ(url.host, "::1");  // brackets stripped: Asio wants the bare address
  EXPECT_EQ(url.port, "8443");
  EXPECT_EQ(url.target, "/ws");
}

TEST(ParseWsUrl, IPv6LiteralWithoutPort) {
  const WsUrl url = Parse("ws://[2001:db8::1]/feed");
  EXPECT_EQ(url.host, "2001:db8::1");
  EXPECT_EQ(url.port, "80");
}

TEST(ParseWsUrl, RejectsMalformed) {
  WsUrl url;
  EXPECT_FALSE(ParseWsUrl("http://example.com", &url));
  EXPECT_FALSE(ParseWsUrl("stream.binance.com", &url));
  EXPECT_FALSE(ParseWsUrl("wss://", &url));
  EXPECT_FALSE(ParseWsUrl("wss://host:/path", &url));
  EXPECT_FALSE(ParseWsUrl("", &url));
}

}  // namespace
}  // namespace md
