// Drives the transport-agnostic Color client core over real HTTP (libcurl).
//
// Serializes access to the non-thread-safe core with a mutex, but performs the
// blocking network POST outside the lock so several requests can be in flight
// concurrently. Retransmission is transport-level: a dropped request or
// response is simply re-POSTed (byte-identically) until a reply arrives, which
// is exactly what the Color exactly-once guarantee is designed for.
#ifndef COLOR_COLOR_HTTP_CLIENT_H
#define COLOR_COLOR_HTTP_CLIENT_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "color_client.h"
#include "fault_http_transport.h"

namespace color {

class ColorHttpClient {
 public:
  // Reports each network attempt so a demo can print the exchange.
  struct AttemptInfo {
    Seq seq;
    Injected injected;   // whether this attempt dropped the request/response
    bool delivered;      // whether a reply came back
    int attempt;         // 1-based attempt number for this request
  };
  using DeliverFn = std::function<void(Seq seq, const std::string& payload)>;
  using AttemptFn = std::function<void(const AttemptInfo&)>;

  ColorHttpClient(std::string url, FaultHttpTransport* transport,
                  DeliverFn on_deliver = {}, bool set_hash = false);

  void on_attempt(AttemptFn fn) { on_attempt_ = std::move(fn); }

  // Generate the next request for `payload` and block, retransmitting on any
  // drop/error, until its response is received. `retry_delay_ms` paces retries.
  void send(const std::string& payload, int retry_delay_ms = 50);

 private:
  static std::vector<std::string> header_lines(const Request& r);

  std::string url_;
  FaultHttpTransport* transport_;
  AttemptFn on_attempt_;
  std::mutex mu_;
  ColorClient core_;  // guarded by mu_
};

}  // namespace color

#endif  // COLOR_COLOR_HTTP_CLIENT_H
