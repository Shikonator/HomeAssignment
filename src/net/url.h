#pragma once

#include <string>
#include <string_view>

namespace md {

// One parser for all four schemes. There were two near-identical copies once,
// broken in opposite IPv6 cases; do not add a third.
struct Url {
  bool secure = false;
  std::string scheme;
  // IPv6 brackets stripped: Asio's resolver wants the bare address.
  std::string host;
  std::string port;
  std::string target = "/";
};

// Port defaults to 443 for wss/https, 80 for ws/http.
bool ParseUrl(std::string_view url, Url* out);

}  // namespace md
