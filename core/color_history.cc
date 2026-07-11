// Implementation of the committed history and rolling hash (color_history.h).
#include "color_history.h"

#include <string_view>

namespace color {
namespace {

// The rolling hash is an FNV-1a 64-bit chain.
constexpr Hash kFnvOffsetBasis = 1469598103934665603ULL;
constexpr Hash kFnvPrime = 1099511628211ULL;

// Fixed label folded into the seed value, and the separator byte placed between
// the previous hash and the event string on each step.
constexpr std::string_view kSeedLabel = "color/v1";
constexpr unsigned char kSeparator = ':';

// Mix one byte into an FNV-1a accumulator.
inline void fnv_mix(Hash& h, unsigned char b) {
  h ^= b;
  h *= kFnvPrime;
}

}  // namespace

std::string Event::str() const {
  return (is_response ? "r" : "R") + std::to_string(seq);
}

Hash hash_seed() {
  Hash h = kFnvOffsetBasis;
  for (char c : kSeedLabel) fnv_mix(h, static_cast<unsigned char>(c));
  return h;
}

Hash hash_step(Hash prev, const Event& e) {
  Hash h = prev;
  // Fold the previous hash in explicitly so order and content both matter.
  for (int i = 0; i < 8; ++i)
    fnv_mix(h, static_cast<unsigned char>((prev >> (8 * i)) & 0xff));
  fnv_mix(h, kSeparator);
  for (char c : e.str()) fnv_mix(h, static_cast<unsigned char>(c));
  return h;
}

Hash History::append(const Event& e) {
  events_.push_back(e);
  cur_hash_ = hash_step(cur_hash_, e);
  return cur_hash_;
}

std::string History::str() const {
  std::string s;
  for (std::size_t i = 0; i < events_.size(); ++i) {
    if (i) s += ' ';
    s += events_[i].str();
  }
  return s;
}

}  // namespace color
