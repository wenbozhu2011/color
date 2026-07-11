// Color verification harness — fuzzy client/server driver.
//
// Wires a ColorClient and ColorServer together over a simulated lossy network
// (drop / duplicate / delay / reorder, seeded) and, for each seed, drives a
// randomized conversation and then asserts the safety & liveness properties via
// checker.h. This is the artifact that exercises the protocol deterministically
// and reproducibly, so a violation shows up as a concrete failing seed.
//
// Usage:
//   color_verify [--seeds N] [--base-seed S] [--steps K] [--parallel P]
//                [--rate f] [--drop f] [--dup f] [--max-latency L] [--rto R]
//                [--minutes M] [--verbose]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "checker.h"
#include "color_client.h"
#include "color_server.h"
#include "sim_network.h"

namespace {

struct Config {
  std::uint64_t seeds = 100;
  std::uint64_t base_seed = 1;
  std::uint64_t gen_steps = 2000;
  std::size_t parallel = 8;  // max outstanding requests (flow-control window)
  double rate = 2.0;         // mean new requests per tick (Poisson arrivals)
  double drop = 0.30;
  double dup = 0.10;
  int max_latency = 5;
  std::uint64_t rto = 6;  // retransmit timeout in ticks
  double minutes = 0.0;   // if > 0, run seeds until this wall-clock budget
  bool verbose = false;
};

struct RunStats {
  color::CheckResult result;
  std::uint64_t steps = 0;
  std::uint64_t issued = 0;
  std::uint64_t req_sent = 0, req_dropped = 0, req_dup = 0;
  std::uint64_t rsp_sent = 0, rsp_dropped = 0, rsp_dup = 0;
  std::size_t max_resp_buffer = 0, max_pending = 0, max_ack_new = 0;
};

// Run one full seeded conversation and check it. Returns per-run stats.
RunStats run_one(std::uint64_t seed, const Config& cfg) {
  std::mt19937_64 rng(seed);
  color::LinkConfig lc{cfg.drop, cfg.dup, 1, cfg.max_latency};
  color::SimLink<color::Request> c2s(lc, rng);
  color::SimLink<color::Response> s2c(lc, rng);

  std::uint64_t now = 0;
  std::uint64_t hash_mismatches = 0;

  auto app = [&now](color::Seq seq, const std::string& /*payload*/,
                    const color::History&) {
    return "srv-ts=" + std::to_string(now) + ";seq=" + std::to_string(seq);
  };
  color::ColorServer server(app, /*set_hash=*/true);
  color::ColorClient client({}, /*set_hash=*/true);
  auto on_mismatch = [&hash_mismatches](color::Seq, color::Hash, color::Hash) {
    ++hash_mismatches;
  };
  server.on_hash_mismatch(on_mismatch);
  client.on_hash_mismatch(on_mismatch);

  std::unordered_map<color::Seq, std::uint64_t> last_send;

  // Requests arrive as a Poisson process: each tick draws Poisson(rate) new
  // requests, capped by the flow-control window (`parallel`). This makes the
  // request interleaving a genuine stochastic process (bursts, gaps, idle
  // ticks) rather than a fixed cadence.
  std::poisson_distribution<int> arrivals(cfg.rate);

  auto send_request = [&](const color::Request& r) {
    c2s.send(now, r);
    last_send[r.seq] = now;
    if (cfg.verbose)
      std::printf("  t=%llu  ->REQ seq=%llu base=%llu new=[%zu]\n",
                  (unsigned long long)now, (unsigned long long)r.seq,
                  (unsigned long long)r.ack_base, r.ack_new.size());
  };

  auto step = [&](bool generating) {
    // 1. Poisson(rate) new requests this tick, capped by the window `parallel`.
    if (generating) {
      int want = arrivals(rng);
      std::size_t out = client.outstanding().size();
      for (int j = 0; j < want && out < cfg.parallel; ++j) {
        std::string p = "cli-ts=" + std::to_string(now) + ";n=" +
                        std::to_string(rng() & 0xffff);
        send_request(client.generate_request(p));
        ++out;
      }
    }
    // 2. Retransmit any outstanding request past its RTO (transport retry).
    for (color::Seq s : client.outstanding()) {
      if (now - last_send[s] >= cfg.rto) send_request(client.frozen(s));
    }
    // 3. Deliver due requests; server may emit responses.
    for (const auto& req : c2s.due(now)) {
      for (const auto& resp : server.on_request(req)) {
        s2c.send(now, resp);
        if (cfg.verbose)
          std::printf("  t=%llu  <-RSP seq=%llu%s\n", (unsigned long long)now,
                      (unsigned long long)resp.seq, resp.no_op ? " (no-op)" : "");
      }
    }
    // 4. Deliver due responses to the client.
    for (const auto& resp : s2c.due(now)) client.on_response(resp);
  };

  // Generation phase.
  for (std::uint64_t i = 0; i < cfg.gen_steps; ++i, ++now) step(/*generating=*/true);

  // Drain phase: stop generating, keep ticking + retransmitting until every
  // request is answered and both links are empty (bounded by a safety cap).
  const std::uint64_t drain_cap = cfg.gen_steps * 50 + 100000;
  std::uint64_t drained = 0;
  while (drained < drain_cap) {
    bool done = client.outstanding().empty() && c2s.empty() && s2c.empty();
    if (done) break;
    step(/*generating=*/false);
    ++now;
    ++drained;
  }

  std::size_t bound = 4 * (cfg.parallel + cfg.max_latency) + 16;
  RunStats st;
  st.result = color::check_run(client, server, hash_mismatches, bound);
  st.steps = now;
  st.issued = client.next_seq() - 1;
  st.req_sent = c2s.sent();
  st.req_dropped = c2s.dropped();
  st.req_dup = c2s.duplicated();
  st.rsp_sent = s2c.sent();
  st.rsp_dropped = s2c.dropped();
  st.rsp_dup = s2c.duplicated();
  st.max_resp_buffer = server.max_resp_buffer();
  st.max_pending = server.max_pending();
  st.max_ack_new = client.max_ack_new();
  return st;
}

Config parse(int argc, char** argv) {
  Config c;
  auto val = [&](int& i) { return std::strtod(argv[++i], nullptr); };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--seeds") c.seeds = (std::uint64_t)val(i);
    else if (a == "--base-seed") c.base_seed = (std::uint64_t)val(i);
    else if (a == "--steps") c.gen_steps = (std::uint64_t)val(i);
    else if (a == "--parallel") c.parallel = (std::size_t)val(i);
    else if (a == "--rate") c.rate = val(i);
    else if (a == "--drop") c.drop = val(i);
    else if (a == "--dup") c.dup = val(i);
    else if (a == "--max-latency") c.max_latency = (int)val(i);
    else if (a == "--rto") c.rto = (std::uint64_t)val(i);
    else if (a == "--minutes") c.minutes = val(i);
    else if (a == "--verbose") c.verbose = true;
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); std::exit(2); }
  }
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse(argc, argv);
  std::printf(
      "Color verification: seeds=%llu steps=%llu parallel=%zu rate=%.2f "
      "drop=%.2f dup=%.2f max_latency=%d rto=%llu%s\n",
      (unsigned long long)cfg.seeds, (unsigned long long)cfg.gen_steps,
      cfg.parallel, cfg.rate, cfg.drop, cfg.dup, cfg.max_latency,
      (unsigned long long)cfg.rto, cfg.minutes > 0 ? " (timed)" : "");

  auto t0 = std::chrono::steady_clock::now();
  std::uint64_t passed = 0, failed = 0, run = 0;
  std::uint64_t tot_issued = 0, tot_req = 0, tot_drop = 0, tot_dup = 0;
  std::size_t worst_buf = 0, worst_pending = 0, worst_ack = 0;

  for (std::uint64_t k = 0;; ++k) {
    if (cfg.minutes <= 0 && k >= cfg.seeds) break;
    if (cfg.minutes > 0) {
      double elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();
      if (elapsed >= cfg.minutes * 60.0) break;
    }
    std::uint64_t seed = cfg.base_seed + k;
    RunStats st = run_one(seed, cfg);
    ++run;
    tot_issued += st.issued;
    tot_req += st.req_sent + st.rsp_sent;
    tot_drop += st.req_dropped + st.rsp_dropped;
    tot_dup += st.req_dup + st.rsp_dup;
    worst_buf = std::max(worst_buf, st.max_resp_buffer);
    worst_pending = std::max(worst_pending, st.max_pending);
    worst_ack = std::max(worst_ack, st.max_ack_new);
    if (st.result.ok) {
      ++passed;
    } else {
      ++failed;
      std::printf("SEED %llu FAILED (issued=%llu):\n", (unsigned long long)seed,
                  (unsigned long long)st.issued);
      for (const auto& f : st.result.failures)
        std::printf("    - %s\n", f.c_str());
    }
  }

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                    .count();
  std::printf("\n---- summary ----\n");
  std::printf("runs=%llu passed=%llu failed=%llu  (%.2fs)\n",
              (unsigned long long)run, (unsigned long long)passed,
              (unsigned long long)failed, secs);
  std::printf("total requests issued : %llu\n", (unsigned long long)tot_issued);
  std::printf("messages sent=%llu dropped=%llu duplicated=%llu\n",
              (unsigned long long)tot_req, (unsigned long long)tot_drop,
              (unsigned long long)tot_dup);
  std::printf("worst-case  resp_buffer=%zu  staging=%zu  ack_new=%zu\n", worst_buf,
              worst_pending, worst_ack);
  std::printf("%s\n", failed == 0 ? "ALL RUNS PASSED" : "FAILURES DETECTED");
  return failed == 0 ? 0 : 1;
}
