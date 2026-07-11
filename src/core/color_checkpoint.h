// Color core — Phase II failover data structures (transport-agnostic).
//
// A `Checkpoint` is the bounded active state a server persists periodically so a
// replacement process can resume. A `Replay` is the client's known history the
// new server asks for (via a 503 recovery signal) to rebuild the events its
// checkpoint lagged behind. Both are plain structs; JSON serialization is a
// transport concern (the simulated harness passes them directly).
#ifndef COLOR_COLOR_CHECKPOINT_H
#define COLOR_COLOR_CHECKPOINT_H

#include <cstddef>
#include <string>
#include <vector>

#include "color_history.h"
#include "color_message.h"

namespace color {

// One committed-but-unacknowledged response retained across failover so the new
// server can resend it to a client still waiting.
struct BufferedResponse {
  Seq seq;
  std::string payload;
  Hash hash;  // the Color-Hash the server echoes for this seq
};

// The persisted active state (docs/failover.md §2).
struct Checkpoint {
  Seq committed_upto = 0;             // highest committed request seq
  std::size_t event_count = 0;        // committed history length (replay boundary)
  Hash history_hash = 0;              // running hash at that frontier
  std::vector<BufferedResponse> buffer;  // committed-but-unacknowledged responses
};

// One event in a replay: a request (R) or a received response (r) with payload.
struct ReplayEvent {
  bool is_response;
  Seq seq;
  std::string payload;  // response body (for r events)
  Hash hash = 0;        // echo hash (for r events); informational
};

// The client's known history from a server-requested position (docs/failover.md
// §4). `from` is a committed-history event index, not a seq: the first `from`
// events are identical on both sides, so replaying the suffix rebuilds the
// server's history in exact order.
struct Replay {
  std::size_t from = 0;
  std::vector<ReplayEvent> events;
};

}  // namespace color

#endif  // COLOR_COLOR_CHECKPOINT_H
