#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

#include <functional>
#include <string>

namespace md {

// One-shot GET on the caller's io_context, so it shares a thread with that
// venue's websocket and needs no synchronisation. `handler` runs exactly once.
using HttpGetHandler = std::function<void(bool ok, std::string body, std::string error)>;

void HttpGet(boost::asio::io_context& io, boost::asio::ssl::context& tls, const std::string& url,
             HttpGetHandler handler);

}  // namespace md
