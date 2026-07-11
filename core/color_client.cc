// Implementation of the Color client state machine (color_client.h).
#include "color_client.h"

namespace color {

Request ColorClient::generate_request(const std::string& p) {
  Seq seq = next_seq_++;
  std::vector<Seq> new_acks = std::move(pending_new_);
  pending_new_.clear();
  if (new_acks.size() > max_ack_new_) max_ack_new_ = new_acks.size();

  for (Seq i : new_acks) history_.append(Event{/*is_response=*/true, i});
  Hash h = history_.append(Event{/*is_response=*/false, seq});
  req_hash_[seq] = h;

  Request r;
  r.seq = seq;
  r.ack_base = base_;
  r.ack_new = new_acks;
  if (set_hash_) r.hash = h;
  r.payload = p;
  inflight_[seq] = r;  // frozen copy for retransmission
  ++sent_;
  return r;
}

bool ColorClient::on_response(const Response& resp) {
  if (received_.count(resp.seq)) return false;  // duplicate delivery
  if (resp.hash && set_hash_) {
    auto it = req_hash_.find(resp.seq);
    if (it != req_hash_.end() && *resp.hash != it->second && on_mismatch_)
      on_mismatch_(resp.seq, *resp.hash, it->second);
  }
  received_.insert(resp.seq);
  if (!resp.no_op) pending_new_.push_back(resp.seq);  // record receipt order
  // Advance the cumulative low-water base over the contiguous received prefix.
  while (received_.count(base_)) ++base_;
  inflight_.erase(resp.seq);  // answered; stop retransmitting
  if (deliver_ && !resp.no_op) deliver_(resp.seq, resp.payload);
  return true;
}

std::vector<Seq> ColorClient::outstanding() const {
  std::vector<Seq> v;
  v.reserve(inflight_.size());
  for (const auto& kv : inflight_) v.push_back(kv.first);
  return v;
}

}  // namespace color
