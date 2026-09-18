#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

#include <functional>
#include <string>

namespace md {

// One-shot asynchronous GET, issued on the caller's io_context so it runs on
// the same thread as that venue's websocket and needs no synchronisation.
//
// `handler` is invoked exactly once. On failure `body` is empty and `error`
// explains why.
using HttpGetHandler = std::function<void(bool ok, std::string body, std::string error)>;

void HttpGet(boost::asio::io_context& io, boost::asio::ssl::context& tls, const std::string& url,
             HttpGetHandler handler);

}  // namespace md
