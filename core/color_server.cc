// Implementation of the Color server state machine (color_server.h).
#include "color_server.h"

namespace color {

std::vector<Response> ColorServer::on_request(const Request& req) {
  std::vector<Response> out;

  // 1. Apply the acknowledgement: release responses the client has confirmed.
  release_acked(req);

  // 2. Exactly-once dedup: a retry of an already-committed request is not
  //    reprocessed; resend the buffered response, or a no-op if released.
  if (committed_.count(req.seq)) {
    auto it = resp_buffer_.find(req.seq);
    if (it != resp_buffer_.end()) {
      out.push_back(make_response(req.seq, it->second, /*no_op=*/false));
    } else {
      out.push_back(make_response(req.seq, "", /*no_op=*/true));
    }
    return out;
  }

  // 3. Stage and advance the committed history in strict id order.
  pending_reqs_[req.seq] = req;
  while (true) {
    auto it = pending_reqs_.find(committed_upto_ + 1);
    if (it == pending_reqs_.end()) break;
    const Request r = it->second;
    pending_reqs_.erase(it);

    for (Seq i : r.ack_new) history_.append(Event{/*is_response=*/true, i});
    Hash h = history_.append(Event{/*is_response=*/false, r.seq});
    req_hash_[r.seq] = h;
    if (r.hash && set_hash_ && *r.hash != h && on_mismatch_)
      on_mismatch_(r.seq, *r.hash, h);

    committed_upto_ = r.seq;
    committed_.insert(r.seq);
    ++app_calls_[r.seq];  // must stay == 1 (exactly-once)

    std::string resp = app_(r.seq, r.payload, history_);
    resp_buffer_[r.seq] = resp;
    out.push_back(make_response(r.seq, resp, /*no_op=*/false));
  }

  track_bounds();
  return out;
}

Response ColorServer::make_response(Seq seq, const std::string& payload,
                                    bool no_op) {
  Response r;
  r.seq = seq;
  r.payload = payload;
  r.no_op = no_op;
  auto it = req_hash_.find(seq);
  if (set_hash_ && it != req_hash_.end()) r.hash = it->second;
  return r;
}

void ColorServer::release_acked(const Request& req) {
  // Drop every buffered response the client has now confirmed: id < ack_base,
  // or id explicitly listed in ack_new.
  for (auto it = resp_buffer_.begin(); it != resp_buffer_.end();) {
    if (it->first < req.ack_base)
      it = resp_buffer_.erase(it);
    else
      ++it;
  }
  for (Seq i : req.ack_new) resp_buffer_.erase(i);
}

void ColorServer::track_bounds() {
  if (resp_buffer_.size() > max_resp_buffer_) max_resp_buffer_ = resp_buffer_.size();
  if (pending_reqs_.size() > max_pending_) max_pending_ = pending_reqs_.size();
}

}  // namespace color
