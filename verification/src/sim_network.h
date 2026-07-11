// Simulated lossy network for the Color verification harness.
//
// A directional link between two Color endpoints that, per a seeded RNG, may
// drop, duplicate, delay, and thereby reorder messages. It stands in for a real
// transport under adverse conditions: loss models the delivery failures the
// client recovers from by retransmitting until a copy gets through.
#ifndef COLOR_SIM_NETWORK_H
#define COLOR_SIM_NETWORK_H

#include <cstdint>
#include <map>
#include <random>
#include <vector>

namespace color {

struct LinkConfig {
  double p_drop = 0.30;   // probability a sent copy is dropped
  double p_dup = 0.10;    // probability a delivered copy is duplicated
  int min_latency = 1;    // inclusive, in ticks
  int max_latency = 5;    // inclusive; spread => reordering
};

// A one-directional lossy link carrying messages of type Msg.
template <typename Msg>
class SimLink {
 public:
  SimLink(LinkConfig cfg, std::mt19937_64& rng) : cfg_(cfg), rng_(rng) {}

  // Offer a message to the link at virtual time `now`. It may be dropped,
  // delivered later, and/or duplicated.
  void send(std::uint64_t now, const Msg& m) {
    ++sent_;
    if (bernoulli(cfg_.p_drop)) {
      ++dropped_;
      return;
    }
    schedule(now, m);
    if (bernoulli(cfg_.p_dup)) {
      ++duplicated_;
      schedule(now, m);  // a second copy, independently delayed
    }
  }

  // Pop every message whose delivery time has arrived by `now`.
  std::vector<Msg> due(std::uint64_t now) {
    std::vector<Msg> out;
    for (auto it = queue_.begin(); it != queue_.end();) {
      if (it->first > now) break;  // multimap is ordered by delivery tick
      out.push_back(it->second);
      ++delivered_;
      it = queue_.erase(it);
    }
    return out;
  }

  bool empty() const { return queue_.empty(); }
  std::uint64_t sent() const { return sent_; }
  std::uint64_t dropped() const { return dropped_; }
  std::uint64_t duplicated() const { return duplicated_; }
  std::uint64_t delivered() const { return delivered_; }

  // For the reliable end-of-run drain: deliver everything with no new loss.
  void set_config(LinkConfig cfg) { cfg_ = cfg; }

 private:
  bool bernoulli(double p) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
  }
  void schedule(std::uint64_t now, const Msg& m) {
    int span = cfg_.max_latency - cfg_.min_latency;
    int extra = span > 0 ? static_cast<int>(rng_() % (span + 1)) : 0;
    std::uint64_t at = now + cfg_.min_latency + extra;
    queue_.emplace(at, m);
  }

  LinkConfig cfg_;
  std::mt19937_64& rng_;
  std::multimap<std::uint64_t, Msg> queue_;
  std::uint64_t sent_ = 0, dropped_ = 0, duplicated_ = 0, delivered_ = 0;
};

}  // namespace color

#endif  // COLOR_SIM_NETWORK_H
