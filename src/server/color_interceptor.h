// Color as a net_http request interceptor: a reusable, framework-level library
// that makes an ordinary (stateless) net_http endpoint speak Color, transparent
// to the application handler.
//
// Registered as a pre-handler RequestInterceptor. For each incoming request it
// parses the Color headers, drives the transport-agnostic server core, and
// completes the HTTP responses for whichever requests the core commits. A
// request that arrives ahead of a gap is *held* (its response deferred) until
// the gap fills — so the response is produced strictly in conversation order,
// exactly as the core requires. The core is not thread-safe, so all access is
// serialized under a mutex; holding request objects until a later request
// completes them is the net_http async-reply pattern.
#ifndef COLOR_COLOR_INTERCEPTOR_H
#define COLOR_COLOR_INTERCEPTOR_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "color_server.h"
#include "net_http/server/public/httpserver_interface.h"
#include "net_http/server/public/server_request_interface.h"

namespace color {

class ColorInterceptor {
 public:
  // Reports each committed request/response so a demo can print the exchange.
  using EventFn = std::function<void(Seq seq, const std::string& payload)>;

  // `app` is the application logic, invoked in committed order (transparent to
  // Color). `set_hash` echoes the optional verification hash in responses.
  explicit ColorInterceptor(ColorServer::AppFn app, bool set_hash = false);

  // Register this interceptor for `uri` on `server`. A no-op fallback handler is
  // also registered so the path is dispatchable; the interceptor returns kExit
  // so the handler never actually runs.
  void Register(net_http::HTTPServerInterface* server, const std::string& uri);

  void on_committed(EventFn fn) { on_committed_ = std::move(fn); }
  void on_hash_mismatch(ColorServer::HashMismatchFn fn) {
    core_.on_hash_mismatch(std::move(fn));
  }

 private:
  net_http::InterceptResult OnRequest(net_http::ServerRequestInterface* req);
  static Request Parse(net_http::ServerRequestInterface* req);
  void Complete(net_http::ServerRequestInterface* req, const Response& resp);

  std::mutex mu_;
  ColorServer core_;  // guarded by mu_
  bool set_hash_;
  EventFn on_committed_;
  // Requests whose HTTP reply is still pending, keyed by seq (guarded by mu_).
  std::unordered_map<Seq, net_http::ServerRequestInterface*> pending_;
};

}  // namespace color

#endif  // COLOR_COLOR_INTERCEPTOR_H
