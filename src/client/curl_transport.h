// A thin libcurl POST transport whose sole added behaviour is failure injection.
//
// Color needs no "rich client": ordinary HTTP POST with transport-level retry
// is enough. This wrapper exists only so a test/demo can inject delivery
// failures on demand — it randomly drops either the outgoing request (the peer
// never sees it) or the incoming response (the peer processed it, but the reply
// is discarded). Everything else is a plain libcurl POST.
#ifndef COLOR_CURL_TRANSPORT_H
#define COLOR_CURL_TRANSPORT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace color {

// Outcome of one POST attempt.
struct HttpResult {
  bool delivered = false;               // true only on a full round trip
  long status = 0;                      // HTTP status code (when delivered)
  std::map<std::string, std::string> headers;  // response headers (lower-cased)
  std::string body;                     // response body (when delivered)
};

// How an attempt failed, for logging.
enum class Injected { kNone, kDropRequest, kDropResponse };

class CurlTransport {
 public:
  // p_drop_request / p_drop_response are per-attempt probabilities. seed makes
  // the injection reproducible; each calling thread derives its own stream.
  CurlTransport(double p_drop_request, double p_drop_response,
                std::uint64_t seed, long timeout_ms = 2000);
  ~CurlTransport();

  // POST `body` to `url` with the given "Key: Value" header lines. On an
  // injected drop the request is either not sent (kDropRequest) or sent and its
  // response discarded (kDropResponse); `injected` reports which. Returns a
  // result with delivered=false on any drop or transport error.
  HttpResult post(const std::string& url,
                  const std::vector<std::string>& header_lines,
                  const std::string& body, Injected* injected);

 private:
  Injected roll();

  double p_drop_request_;
  double p_drop_response_;
  std::uint64_t seed_;
  long timeout_ms_;
};

}  // namespace color

#endif  // COLOR_CURL_TRANSPORT_H
