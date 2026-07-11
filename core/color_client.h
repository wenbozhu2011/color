// Color core — client state machine, transport-agnostic.
//
// The client assigns contiguous request ids, freezes each request (so retries
// are byte-identical), records the receipt order of responses, and commits its
// local history in the canonical total order. It knows nothing about HTTP or
// the network; a transport calls generate_request()/on_response() and moves the
// bytes. Retransmission is a transport concern — the transport asks the client
// for frozen(seq) to resend.
#ifndef COLOR_COLOR_CLIENT_H
#define COLOR_COLOR_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "color_history.h"
#include "color_message.h"

namespace color {

class ColorClient {
 public:
  // Called (in receipt order, once per seq) when a response is first received.
  using DeliverFn = std::function<void(Seq seq, const std::string& payload)>;
  // Called when a piggybacked response hash disagrees with the local history.
  using HashMismatchFn = std::function<void(Seq seq, Hash got, Hash expected)>;

  explicit ColorClient(DeliverFn deliver = {}, bool set_hash = true)
      : deliver_(std::move(deliver)), set_hash_(set_hash) {}

  void on_hash_mismatch(HashMismatchFn fn) { on_mismatch_ = std::move(fn); }

  // Generate the next request for application payload `p`. Commits the frozen
  // receipt-ordered delta and this request into the local history.
  Request generate_request(const std::string& p);

  // Process a received response. Duplicate deliveries are ignored. Returns true
  // if this was the first receipt of `resp.seq`.
  bool on_response(const Response& resp);

  // ---- transport / driver queries ----
  std::vector<Seq> outstanding() const;
  const Request& frozen(Seq seq) const { return inflight_.at(seq); }
  bool has_response(Seq seq) const { return received_.count(seq) != 0; }

  // ---- checker queries ----
  const History& history() const { return history_; }
  const std::unordered_set<Seq>& received() const { return received_; }
  Seq next_seq() const { return next_seq_; }
  std::uint64_t sent() const { return sent_; }
  std::size_t max_ack_new() const { return max_ack_new_; }

 private:
  DeliverFn deliver_;
  HashMismatchFn on_mismatch_;
  bool set_hash_;

  Seq next_seq_ = 1;
  Seq base_ = 1;
  std::unordered_set<Seq> received_;
  std::vector<Seq> pending_new_;
  std::unordered_map<Seq, Hash> req_hash_;
  std::unordered_map<Seq, Request> inflight_;
  History history_;

  std::uint64_t sent_ = 0;
  std::size_t max_ack_new_ = 0;
};

}  // namespace color

#endif  // COLOR_COLOR_CLIENT_H
