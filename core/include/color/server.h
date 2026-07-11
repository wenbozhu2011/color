// Color core — server state machine (docs/protocol.md §6), transport-agnostic.
//
// The server dedups requests by seq (exactly-once), releases buffered responses
// as the client acknowledges them, and commits the shared history in strict id
// order (parking out-of-order arrivals until their gaps fill). The application
// is invoked in committed order and sees the ordered "conversation state".
// on_request() returns the responses the transport should now send.
#ifndef COLOR_SERVER_H
#define COLOR_SERVER_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "color/history.h"
#include "color/message.h"

namespace color {

class ColorServer {
 public:
  // The reference application: given a committed request, produce a response
  // payload. Invoked at most once per seq, in committed (id) order.
  using AppFn = std::function<std::string(Seq seq, const std::string& payload,
                                          const History& committed)>;
  using HashMismatchFn = std::function<void(Seq seq, Hash got, Hash expected)>;

  explicit ColorServer(AppFn app, bool set_hash = true)
      : app_(std::move(app)), set_hash_(set_hash) {}

  void on_hash_mismatch(HashMismatchFn fn) { on_mismatch_ = std::move(fn); }

  // Handle a (possibly duplicated / out-of-order) request. Returns responses to
  // transmit (0..N: newly committed requests, or a resend/no-op for a retry).
  std::vector<Response> on_request(const Request& req) {
    std::vector<Response> out;

    // 1. Apply the acknowledgement: release what the client has confirmed (D4).
    release_acked(req);

    // 2. Exactly-once dedup: a retry of an already-committed request is not
    //    reprocessed; resend the buffered response, or a no-op if released.
    if (committed_.count(req.seq)) {
      auto it = resp_buffer_.find(req.seq);
      if (it != resp_buffer_.end()) {
        out.push_back(make_response(req.seq, it->second, /*no_op=*/false));
      } else {
        out.push_back(make_response(req.seq, "", /*no_op=*/true));
      }
      return out;
    }

    // 3. Stage and advance the committed history in strict id order.
    pending_reqs_[req.seq] = req;
    while (true) {
      auto it = pending_reqs_.find(committed_upto_ + 1);
      if (it == pending_reqs_.end()) break;
      const Request r = it->second;
      pending_reqs_.erase(it);

      for (Seq i : r.ack_new) history_.append(Token{/*is_response=*/true, i});
      Hash h = history_.append(Token{/*is_response=*/false, r.seq});
      req_hash_[r.seq] = h;
      if (r.hash && set_hash_ && *r.hash != h && on_mismatch_)
        on_mismatch_(r.seq, *r.hash, h);

      committed_upto_ = r.seq;
      committed_.insert(r.seq);
      ++app_calls_[r.seq];  // must stay == 1 (exactly-once)

      std::string resp = app_(r.seq, r.payload, history_);
      resp_buffer_[r.seq] = resp;
      out.push_back(make_response(r.seq, resp, /*no_op=*/false));
    }

    track_bounds();
    return out;
  }

  // ---- checker queries ----
  const History& history() const { return history_; }
  Seq committed_upto() const { return committed_upto_; }
  const std::unordered_map<Seq, int>& app_calls() const { return app_calls_; }
  std::size_t max_resp_buffer() const { return max_resp_buffer_; }
  std::size_t max_pending() const { return max_pending_; }
  std::size_t resp_buffer_size() const { return resp_buffer_.size(); }

 private:
  Response make_response(Seq seq, const std::string& payload, bool no_op) {
    Response r;
    r.seq = seq;
    r.payload = payload;
    r.no_op = no_op;
    auto it = req_hash_.find(seq);
    if (set_hash_ && it != req_hash_.end()) r.hash = it->second;
    return r;
  }

  void release_acked(const Request& req) {
    // Drop every buffered response the client has now confirmed: id < ack_base,
    // or id explicitly listed in ack_new.
    for (auto it = resp_buffer_.begin(); it != resp_buffer_.end();) {
      if (it->first < req.ack_base)
        it = resp_buffer_.erase(it);
      else
        ++it;
    }
    for (Seq i : req.ack_new) resp_buffer_.erase(i);
  }

  void track_bounds() {
    if (resp_buffer_.size() > max_resp_buffer_) max_resp_buffer_ = resp_buffer_.size();
    if (pending_reqs_.size() > max_pending_) max_pending_ = pending_reqs_.size();
  }

  AppFn app_;
  HashMismatchFn on_mismatch_;
  bool set_hash_;

  Seq committed_upto_ = 0;
  std::unordered_map<Seq, Request> pending_reqs_;
  std::unordered_map<Seq, Hash> req_hash_;
  std::unordered_map<Seq, std::string> resp_buffer_;
  std::unordered_set<Seq> committed_;  // seqs already committed
  std::unordered_map<Seq, int> app_calls_;   // seq -> times app invoked
  History history_;

  std::size_t max_resp_buffer_ = 0;
  std::size_t max_pending_ = 0;
};

}  // namespace color

#endif  // COLOR_SERVER_H
