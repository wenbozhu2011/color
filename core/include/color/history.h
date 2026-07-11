// Color core — committed history and its rolling hash (docs/protocol.md §4, §7).
//
// The committed history is one interleaved sequence of request tokens (R<seq>)
// and response tokens (r<seq>). Both client and server build it by the same
// deterministic procedure so that, on their common prefix, the two histories
// are identical token-for-token (the safety invariant).
//
// The rolling hash chains over the token stream:  h_k = Hash(h_{k-1}, token_k).
// docs/protocol.md specifies SHA-256 truncated; for the self-contained
// prototype we use a 64-bit FNV-1a-based chaining hash of identical *structure*
// (h_k depends only on h_{k-1} and the token). The harness never relies on the
// hash alone — it also deep-compares the full histories — so this stand-in is
// sufficient to detect divergence at the exact token.
#ifndef COLOR_HISTORY_H
#define COLOR_HISTORY_H

#include <cstdint>
#include <string>
#include <vector>

#include "color/message.h"

namespace color {

// One entry in the total-ordered history.
struct Token {
  bool is_response;  // false => R<seq> (request), true => r<seq> (response)
  Seq seq;

  bool operator==(const Token& o) const {
    return is_response == o.is_response && seq == o.seq;
  }
  bool operator!=(const Token& o) const { return !(*this == o); }

  // Wire form used both for display and as the hash input, e.g. "R7" / "r7".
  std::string str() const {
    return (is_response ? "r" : "R") + std::to_string(seq);
  }
};

// Seed for the rolling hash (the H0 of docs/protocol.md §7).
inline Hash hash_seed() {
  // FNV-1a 64-bit offset basis, mixed with a fixed label.
  Hash h = 1469598103934665603ULL;
  for (char c : std::string("color/v1")) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// h_k = Hash(h_{k-1}, token): chain the previous hash and the token string.
inline Hash hash_step(Hash prev, const Token& t) {
  Hash h = prev;
  auto mix = [&h](unsigned char b) {
    h ^= b;
    h *= 1099511628211ULL;
  };
  // Fold the previous hash in explicitly so order/content both matter.
  for (int i = 0; i < 8; ++i) mix(static_cast<unsigned char>((prev >> (8 * i)) & 0xff));
  mix(static_cast<unsigned char>(':'));
  for (char c : t.str()) mix(static_cast<unsigned char>(c));
  return h;
}

// The committed history plus the running hash and the {token -> hash} map that
// docs/protocol.md §7 (D5) describes. Shared by client and server.
class History {
 public:
  History() : cur_hash_(hash_seed()) {}

  // Append a token, advancing the rolling hash. Returns the hash *after* this
  // token (i.e. hashmap[token]).
  Hash append(const Token& t) {
    tokens_.push_back(t);
    cur_hash_ = hash_step(cur_hash_, t);
    return cur_hash_;
  }

  const std::vector<Token>& tokens() const { return tokens_; }
  std::size_t size() const { return tokens_.size(); }
  Hash cur_hash() const { return cur_hash_; }

  // Human-readable form, e.g. "R1 r1 R2 R3 r3 r2 R4".
  std::string str() const {
    std::string s;
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (i) s += ' ';
      s += tokens_[i].str();
    }
    return s;
  }

 private:
  std::vector<Token> tokens_;
  Hash cur_hash_;
};

}  // namespace color

#endif  // COLOR_HISTORY_H
