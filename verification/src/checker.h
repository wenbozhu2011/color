// Correctness checker for the Color verification harness.
//
// Verifies, after a fuzzed run, the safety and liveness properties:
//   - SAFETY       : the server's committed history is an exact prefix of the
//                    client's (both sides agree on one total order).
//   - EXACTLY-ONCE : the application ran exactly once per committed seq.
//   - HASH         : no piggybacked history hash ever disagreed.
//   - LIVENESS     : after draining, every request got a response and the
//                    server committed all of them.
//   - BOUNDED      : retransmission buffer, staging, and ack size stayed within
//                    a window-derived bound (independent of run length).
#ifndef COLOR_CHECKER_H
#define COLOR_CHECKER_H

#include <cstdint>
#include <string>
#include <vector>

#include "color_client.h"
#include "color_server.h"

namespace color {

struct CheckResult {
  bool ok = true;
  std::vector<std::string> failures;
  void require(bool cond, const std::string& msg) {
    if (!cond) {
      ok = false;
      failures.push_back(msg);
    }
  }
};

// `hash_mismatches` is the count accumulated by the client/server mismatch
// callbacks during the run. `bound` is the allowed max for buffer/staging/ack.
// `had_failover` relaxes the checks that a rebuilt server cannot satisfy (it
// retains only the post-checkpoint suffix of the history and processed only the
// seqs it committed itself); safety across a failover is proven by hash
// agreement instead.
inline CheckResult check_run(const ColorClient& client, const ColorServer& server,
                             std::uint64_t hash_mismatches, std::size_t bound,
                             bool had_failover = false) {
  CheckResult r;

  // SAFETY (no failover): server history is an exact prefix of client history.
  if (!had_failover) {
    const auto& ch = client.history().events();
    const auto& sh = server.history().events();
    r.require(sh.size() <= ch.size(),
              "server history longer than client history (" +
                  std::to_string(sh.size()) + " > " + std::to_string(ch.size()) + ")");
    std::size_t common = std::min(sh.size(), ch.size());
    for (std::size_t i = 0; i < common; ++i) {
      if (sh[i] != ch[i]) {
        r.require(false, "history divergence at event " + std::to_string(i) +
                             ": server=" + sh[i].str() + " client=" + ch[i].str());
        break;
      }
    }
  }

  // SAFETY (always): the client and server agree on the final history hash.
  // This is the authoritative cross-failover safety check — after a replay the
  // rebuilt server's hash re-converges to the client's.
  r.require(client.history().cur_hash() == server.history().cur_hash(),
            "final history hash mismatch (client vs server)");

  // HASH: no piggybacked hash ever disagreed during the run.
  r.require(hash_mismatches == 0,
            "hash mismatches detected: " + std::to_string(hash_mismatches));

  // EXACTLY-ONCE: the app ran at most once per seq. Without a failover the app
  // ran for exactly the committed seqs; a rebuilt server only ran the app for
  // the seqs it committed itself, so its count is a subset.
  Seq up = server.committed_upto();
  if (!had_failover) {
    r.require(server.app_calls().size() == up,
              "app_calls size " + std::to_string(server.app_calls().size()) +
                  " != committed_upto " + std::to_string(up));
  }
  for (const auto& kv : server.app_calls()) {
    r.require(kv.second == 1, "seq " + std::to_string(kv.first) + " processed " +
                                  std::to_string(kv.second) + " times (want 1)");
  }

  // LIVENESS: after drain, every issued request is answered and committed.
  Seq issued = client.next_seq() - 1;  // ids are 1..issued
  r.require(server.committed_upto() == issued,
            "committed_upto " + std::to_string(server.committed_upto()) +
                " != issued " + std::to_string(issued));
  for (Seq s = 1; s <= issued; ++s) {
    r.require(client.received().count(s) != 0,
              "request " + std::to_string(s) + " never answered");
  }

  // BOUNDED: buffers/staging/ack stayed within the window bound.
  r.require(server.max_resp_buffer() <= bound,
            "resp buffer max " + std::to_string(server.max_resp_buffer()) +
                " exceeds bound " + std::to_string(bound));
  r.require(server.max_pending() <= bound,
            "pending staging max " + std::to_string(server.max_pending()) +
                " exceeds bound " + std::to_string(bound));
  r.require(client.max_ack_new() <= bound,
            "ack-new max " + std::to_string(client.max_ack_new()) +
                " exceeds bound " + std::to_string(bound));
  return r;
}

}  // namespace color

#endif  // COLOR_CHECKER_H
