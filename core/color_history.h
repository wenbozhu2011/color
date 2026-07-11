// Color core — committed history and its rolling hash (docs/protocol.md §4, §7).
//
// The committed history is one interleaved sequence of request events (R<seq>)
// and response events (r<seq>). Both client and server build it by the same
// deterministic procedure so that, on their common prefix, the two histories
// are identical event-for-event (the safety invariant).
//
// The rolling hash chains over the event stream:  h_k = hash_step(h_{k-1}, e_k).
// docs/protocol.md specifies SHA-256 truncated; for the self-contained
// prototype we use a 64-bit FNV-1a-based chaining hash of identical *structure*
// (h_k depends only on h_{k-1} and the event). The harness never relies on the
// hash alone — it also deep-compares the full histories — so this stand-in is
// sufficient to detect divergence at the exact event.
#ifndef COLOR_COLOR_HISTORY_H
#define COLOR_COLOR_HISTORY_H

#include <cstdint>
#include <string>
#include <vector>

#include "color_message.h"

namespace color {

// One entry in the total-ordered history.
struct Event {
  bool is_response;  // false => R<seq> (request), true => r<seq> (response)
  Seq seq;

  bool operator==(const Event& o) const {
    return is_response == o.is_response && seq == o.seq;
  }
  bool operator!=(const Event& o) const { return !(*this == o); }

  // Wire form used both for display and as the hash input, e.g. "R7" / "r7".
  std::string str() const;
};

// Seed for the rolling hash (the H0 of docs/protocol.md §7).
Hash hash_seed();

// h_k = hash_step(h_{k-1}, event): chain the previous hash and the event string.
Hash hash_step(Hash prev, const Event& e);

// The committed history plus the running hash and the {event -> hash} map that
// docs/protocol.md §7 (D5) describes. Shared by client and server.
class History {
 public:
  History() : cur_hash_(hash_seed()) {}

  // Append an event, advancing the rolling hash. Returns the hash *after* this
  // event (i.e. hashmap[event]).
  Hash append(const Event& e);

  const std::vector<Event>& events() const { return events_; }
  std::size_t size() const { return events_.size(); }
  Hash cur_hash() const { return cur_hash_; }

  // Human-readable form, e.g. "R1 r1 R2 R3 r3 r2 R4".
  std::string str() const;

 private:
  std::vector<Event> events_;
  Hash cur_hash_;
};

}  // namespace color

#endif  // COLOR_COLOR_HISTORY_H
