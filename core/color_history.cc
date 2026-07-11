// Implementation of the committed history and rolling hash (color_history.h).
#include "color_history.h"

namespace color {

std::string Event::str() const {
  return (is_response ? "r" : "R") + std::to_string(seq);
}

Hash hash_seed() {
  // FNV-1a 64-bit offset basis, mixed with a fixed label.
  Hash h = 1469598103934665603ULL;
  for (char c : std::string("color/v1")) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

Hash hash_step(Hash prev, const Event& e) {
  Hash h = prev;
  auto mix = [&h](unsigned char b) {
    h ^= b;
    h *= 1099511628211ULL;
  };
  // Fold the previous hash in explicitly so order/content both matter.
  for (int i = 0; i < 8; ++i)
    mix(static_cast<unsigned char>((prev >> (8 * i)) & 0xff));
  mix(static_cast<unsigned char>(':'));
  for (char c : e.str()) mix(static_cast<unsigned char>(c));
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
