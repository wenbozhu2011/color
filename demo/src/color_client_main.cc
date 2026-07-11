// Color client program: drives a conversation against a Color HTTP server,
// injecting request/response drops and retransmitting, printing each event.
//
// Doubles as the demo client — run it slowed (default ~1 msg/sec) against a
// net_http Color server and watch the "chat" continue across dropped messages
// (and, in the failover demo, across a server restart).
//
// Usage:
//   color_client [--url U] [--count N] [--interval-ms M] [--parallel P]
//                [--drop f] [--drop-resp f] [--seed S] [--hash] [--quiet]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "color_http_client.h"
#include "curl_transport.h"

namespace {

struct Config {
  std::string url = "http://127.0.0.1:8080/color";
  int count = 20;
  int interval_ms = 1000;  // pacing between request generations (per sender)
  int parallel = 1;        // concurrent sender threads
  double drop = 0.30;
  double drop_resp = 0.30;
  std::uint64_t seed = 1;
  bool set_hash = false;
  bool quiet = false;
};

const char* inj_str(color::Injected i) {
  switch (i) {
    case color::Injected::kDropRequest: return "drop-request";
    case color::Injected::kDropResponse: return "drop-response";
    default: return "ok";
  }
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer so redirected logs flush
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](int& i) { return std::string(argv[++i]); };
    if (a == "--url") c.url = next(i);
    else if (a == "--count") c.count = std::stoi(next(i));
    else if (a == "--interval-ms") c.interval_ms = std::stoi(next(i));
    else if (a == "--parallel") c.parallel = std::stoi(next(i));
    else if (a == "--drop") c.drop = std::stod(next(i));
    else if (a == "--drop-resp") c.drop_resp = std::stod(next(i));
    else if (a == "--seed") c.seed = std::stoull(next(i));
    else if (a == "--hash") c.set_hash = true;
    else if (a == "--quiet") c.quiet = true;
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
  }

  std::printf("color_client -> %s  count=%d interval=%dms parallel=%d "
              "drop=%.2f drop_resp=%.2f\n",
              c.url.c_str(), c.count, c.interval_ms, c.parallel, c.drop,
              c.drop_resp);

  color::CurlTransport transport(c.drop, c.drop_resp, c.seed);

  auto on_deliver = [&](color::Seq seq, const std::string& payload) {
    if (!c.quiet)
      std::printf("  [recv] seq=%llu  <- %s\n", (unsigned long long)seq,
                  payload.c_str());
  };
  color::ColorHttpClient client(c.url, &transport, on_deliver, c.set_hash);
  std::atomic<int> mismatches{0};
  client.on_hash_mismatch([&](color::Seq seq, color::Hash got, color::Hash exp) {
    ++mismatches;
    std::printf("  [HASH MISMATCH] seq=%llu got=%llu expected=%llu\n",
                (unsigned long long)seq, (unsigned long long)got,
                (unsigned long long)exp);
  });
  client.on_attempt([&](const color::ColorHttpClient::AttemptInfo& a) {
    if (!c.quiet && (a.injected != color::Injected::kNone || a.attempt > 1))
      std::printf("  [send] seq=%llu  attempt=%d  %s%s\n",
                  (unsigned long long)a.seq, a.attempt, inj_str(a.injected),
                  a.delivered ? " (delivered)" : "");
  });
  client.on_recover([&](std::size_t from, std::size_t events) {
    std::printf("  [recover] server failed over -> replayed %zu history events "
                "from index %zu\n", events, from);
  });

  std::atomic<int> next_id{0};
  auto worker = [&]() {
    while (true) {
      int id = next_id.fetch_add(1);
      if (id >= c.count) break;
      auto now = std::chrono::system_clock::now().time_since_epoch();
      long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
      std::string payload = "{\"cli_ts\":" + std::to_string(ms) + "}";
      if (!c.quiet)
        std::printf("[send] msg #%d -> %s\n", id, payload.c_str());
      client.send(payload);
      std::this_thread::sleep_for(std::chrono::milliseconds(c.interval_ms));
    }
  };

  std::vector<std::thread> pool;
  for (int i = 0; i < c.parallel; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  std::printf("done: %d messages exchanged, hash mismatches=%d\n", c.count,
              mismatches.load());
  return mismatches.load() == 0 ? 0 : 1;
}
