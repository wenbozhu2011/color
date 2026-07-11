// Color core — wire message model (transport-agnostic).
//
// These structs are what a transport (an in-process simulated network, or a
// real HTTP client/server) moves between the client and server. The Color
// metadata travels as HTTP headers:
//   Request : Color-Seq, Color-Ack-Base, Color-Ack-New, Color-Hash(optional)
//   Response: Color-Seq(echo), Color-Hash(optional)
// The application payload is an opaque string (JSON in the reference app).
#ifndef COLOR_COLOR_MESSAGE_H
#define COLOR_COLOR_MESSAGE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace color {

using Seq = std::uint64_t;
using Hash = std::uint64_t;  // rolling history hash (see color_history.h)

// A Color request (an HTTP POST carrying the headers above + a body).
struct Request {
  Seq seq = 0;                 // Color-Seq: contiguous, monotonic request id
  Seq ack_base = 1;            // Color-Ack-Base: all responses < ack_base received
  std::vector<Seq> ack_new;    // Color-Ack-New: responses received since the
                               //   previous request, in receipt order
  std::optional<Hash> hash;    // Color-Hash: optional, verification only
  std::string payload;         // request body (opaque to Color)
};

// A Color response (the HTTP response to a request's POST).
struct Response {
  Seq seq = 0;                 // Color-Seq: echoes the request id answered
  std::optional<Hash> hash;    // Color-Hash: optional, verification only
  std::string payload;         // response body (opaque to Color)
  bool no_op = false;          // true => acknowledgement-only reply (empty body)
                               //   sent when a duplicate request arrives after
                               //   its response was already released
};

}  // namespace color

#endif  // COLOR_COLOR_MESSAGE_H
