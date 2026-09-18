#include "src/net/url.h"

namespace md {
namespace {

struct Scheme {
  std::string_view prefix;
  std::string_view name;
  bool secure;
  std::string_view default_port;
};

constexpr Scheme kSchemes[] = {
    {"wss://", "wss", true, "443"},
    {"ws://", "ws", false, "80"},
    {"https://", "https", true, "443"},
    {"http://", "http", false, "80"},
};

}  // namespace

bool ParseUrl(std::string_view url, Url* out) {
  const Scheme* scheme = nullptr;
  for (const Scheme& candidate : kSchemes) {
    if (url.starts_with(candidate.prefix)) {
      scheme = &candidate;
      url.remove_prefix(candidate.prefix.size());
      break;
    }
  }
  if (scheme == nullptr) return false;

  out->scheme = std::string(scheme->name);
  out->secure = scheme->secure;

  const std::size_t slash = url.find('/');
  const std::string_view authority = url.substr(0, slash);
  out->target = (slash == std::string_view::npos) ? "/" : std::string(url.substr(slash));
  if (authority.empty()) return false;

  // The port separator is the first colon AFTER any closing bracket. A bracketed
  // IPv6 literal contains colons of its own, so searching the whole authority
  // finds one of those instead -- which is how the two previous parsers each
  // went wrong, in opposite directions.
  const std::size_t bracket = authority.rfind(']');
  const std::size_t search_from = (bracket == std::string_view::npos) ? 0 : bracket + 1;
  const std::size_t colon = authority.find(':', search_from);

  if (colon != std::string_view::npos) {
    out->host = std::string(authority.substr(0, colon));
    out->port = std::string(authority.substr(colon + 1));
    if (out->port.empty()) return false;
  } else {
    out->host = std::string(authority);
    out->port = std::string(scheme->default_port);
  }

  if (out->host.size() >= 2 && out->host.front() == '[' && out->host.back() == ']') {
    out->host = out->host.substr(1, out->host.size() - 2);
  }
  return !out->host.empty();
}

}  // namespace md
