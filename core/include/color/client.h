// Color core — client state machine (docs/protocol.md §5), transport-agnostic.
//
// The client assigns contiguous request ids, freezes each request (so retries
// are byte-identical), records the receipt order of responses, and commits its
// local history in the canonical total order. It knows nothing about HTTP or
// the network; a transport calls generate_request()/on_response() and moves the
// bytes. Retransmission is a transport concern — the transport asks the client
// for frozen(seq) to resend.
#ifndef COLOR_CLIENT_H
#define COLOR_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "color/history.h"
#include "color/message.h"

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
  Request generate_request(const std::string& p) {
    Seq seq = next_seq_++;
    std::vector<Seq> new_acks = std::move(pending_new_);
    pending_new_.clear();

    for (Seq i : new_acks) history_.append(Token{/*is_response=*/true, i});
    Hash h = history_.append(Token{/*is_response=*/false, seq});
    req_hash_[seq] = h;

    Request r;
    r.seq = seq;
    r.ack_base = base_;
    r.ack_new = new_acks;
    if (set_hash_) r.hash = h;
    r.payload = p;
    inflight_[seq] = r;  // frozen copy for retransmission
    ++sent_;
    return r;
  }

  // Process a received response. Duplicate deliveries are ignored. Returns true
  // if this was the first receipt of `resp.seq`.
  bool on_response(const Response& resp) {
    if (received_.count(resp.seq)) return false;  // duplicate delivery
    if (resp.hash && set_hash_) {
      auto it = req_hash_.find(resp.seq);
      if (it != req_hash_.end() && *resp.hash != it->second && on_mismatch_)
        on_mismatch_(resp.seq, *resp.hash, it->second);
    }
    received_.insert(resp.seq);
    if (!resp.no_op) pending_new_.push_back(resp.seq);  // record receipt order
    // Advance the cumulative low-water base over the contiguous received prefix.
    while (received_.count(base_)) ++base_;
    inflight_.erase(resp.seq);  // answered; stop retransmitting
    if (deliver_ && !resp.no_op) deliver_(resp.seq, resp.payload);
    return true;
  }

  // ---- transport / driver queries ----
  std::vector<Seq> outstanding() const {
    std::vector<Seq> v;
    v.reserve(inflight_.size());
    for (const auto& kv : inflight_) v.push_back(kv.first);
    return v;
  }
  const Request& frozen(Seq seq) const { return inflight_.at(seq); }
  bool has_response(Seq seq) const { return received_.count(seq) != 0; }

  // ---- checker queries ----
  const History& history() const { return history_; }
  const std::unordered_set<Seq>& received() const { return received_; }
  Seq next_seq() const { return next_seq_; }
  std::uint64_t sent() const { return sent_; }
  std::size_t max_ack_new() const { return max_ack_new_; }

  Request generate_and_track(const std::string& p) {
    Request r = generate_request(p);
    if (r.ack_new.size() > max_ack_new_) max_ack_new_ = r.ack_new.size();
    return r;
  }

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

#endif  // COLOR_CLIENT_H
