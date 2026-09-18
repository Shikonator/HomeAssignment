#pragma once

#include <string>
#include <string_view>

namespace md {

// A parsed ws://, wss://, http:// or https:// URL.
//
// One parser for all four schemes, deliberately. There were previously two
// near-identical implementations, one in the WebSocket client and one in the
// HTTP client, and they were broken in *opposite* cases: one mishandled an IPv6
// literal with an explicit port, the other an IPv6 literal without one. They
// also disagreed about whether to strip the brackets. Two copies of a fiddly
// string function is how that happens; one copy with tests is the fix.
struct Url {
  bool secure = false;
  std::string scheme;
  // Bare host. Brackets are stripped from IPv6 literals, because that is what
  // Asio's resolver wants -- the brackets are URL syntax, not part of the
  // address.
  std::string host;
  std::string port;
  std::string target = "/";
};

// Returns false on an unknown scheme, an empty host, or an empty explicit port.
// Port defaults to 443 for wss/https and 80 for ws/http.
bool ParseUrl(std::string_view url, Url* out);

}  // namespace md
