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
inline CheckResult check_run(const ColorClient& client, const ColorServer& server,
                             std::uint64_t hash_mismatches, std::size_t bound) {
  CheckResult r;

  // SAFETY: server history is an exact prefix of client history.
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

  // HASH: no piggybacked hash ever disagreed.
  r.require(hash_mismatches == 0,
            "hash mismatches detected: " + std::to_string(hash_mismatches));

  // EXACTLY-ONCE: app invoked exactly once per committed seq 1..committed_upto.
  Seq up = server.committed_upto();
  r.require(server.app_calls().size() == up,
            "app_calls size " + std::to_string(server.app_calls().size()) +
                " != committed_upto " + std::to_string(up));
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
