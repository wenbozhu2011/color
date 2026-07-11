// Color core — server state machine, transport-agnostic.
//
// The server dedups requests by seq (exactly-once), releases buffered responses
// as the client acknowledges them, and commits the shared history in strict id
// order (parking out-of-order arrivals until their gaps fill). The application
// is invoked in committed order and sees the ordered "conversation state".
// on_request() returns the responses the transport should now send.
//
// Failover: a server can be built from a Checkpoint (the persisted active
// state) and, when it detects a request acknowledging history it lacks, asks the
// client to replay that history (a 503 recovery signal) and rebuilds via
// ingest_replay().
#ifndef COLOR_COLOR_SERVER_H
#define COLOR_COLOR_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "color_checkpoint.h"
#include "color_history.h"
#include "color_message.h"

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

  // Resume from a checkpoint after a failover.
  ColorServer(const Checkpoint& cp, AppFn app, bool set_hash = true);

  void on_hash_mismatch(HashMismatchFn fn) { on_mismatch_ = std::move(fn); }

  // Handle a (possibly duplicated / out-of-order) request. Returns responses to
  // transmit. A single response with `recover == true` is a 503 recovery signal
  // (the client should replay from `recover_from` and resend).
  std::vector<Response> on_request(const Request& req);

  // ---- failover ----
  // Snapshot the active state for persistence.
  Checkpoint checkpoint() const;
  // Rebuild committed history/hash from a client replay.
  void ingest_replay(const Replay& rp);

  // ---- checker queries ----
  const History& history() const { return history_; }
  Seq committed_upto() const { return committed_upto_; }
  const std::unordered_map<Seq, int>& app_calls() const { return app_calls_; }
  std::size_t max_resp_buffer() const { return max_resp_buffer_; }
  std::size_t max_pending() const { return max_pending_; }
  std::size_t resp_buffer_size() const { return resp_buffer_.size(); }

 private:
  Response make_response(Seq seq, const std::string& payload, bool no_op);
  void release_acked(const Request& req);
  bool needs_recover(const Request& req) const;
  void track_bounds();

  AppFn app_;
  HashMismatchFn on_mismatch_;
  bool set_hash_;

  Seq committed_upto_ = 0;
  std::unordered_map<Seq, Request> pending_reqs_;
  std::unordered_map<Seq, Hash> req_hash_;
  std::unordered_map<Seq, std::string> resp_buffer_;
  // Replayed requests whose response the client is still missing: (re)process
  // them once when the client retransmits.
  std::unordered_set<Seq> awaiting_;
  std::unordered_map<Seq, int> app_calls_;   // seq -> times app invoked
  History history_;

  std::size_t max_resp_buffer_ = 0;
  std::size_t max_pending_ = 0;
};

}  // namespace color

#endif  // COLOR_COLOR_SERVER_H
