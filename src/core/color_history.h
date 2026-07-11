// Color core — committed history and its rolling hash.
//
// The committed history is one interleaved sequence of request events (R<seq>)
// and response events (r<seq>). Both the client and the server build it by the
// same deterministic procedure, so that on their common prefix the two
// histories are identical event-for-event (the core safety invariant).
//
// The rolling hash chains over the event stream: h_k = hash_step(h_{k-1}, e_k),
// a 64-bit FNV-1a chain (keeps the core dependency-free). It lets the two peers
// cross-check that their histories agree, but carries nothing the protocol
// needs to function; each side may ignore it.
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

// Seed for the rolling hash (the initial value h_0).
Hash hash_seed();

// h_k = hash_step(h_{k-1}, event): chain the previous hash and the event string.
Hash hash_step(Hash prev, const Event& e);

// The committed history plus its running rolling hash. Shared by client and
// server.
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
