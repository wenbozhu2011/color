// Implementation of the Color server state machine (color_server.h).
#include "color_server.h"

namespace color {

ColorServer::ColorServer(const Checkpoint& cp, AppFn app, bool set_hash)
    : app_(std::move(app)),
      set_hash_(set_hash),
      committed_upto_(cp.committed_upto),
      history_(cp.history_hash, cp.event_count) {
  // Restore the committed-but-unacknowledged responses so retries of a still-
  // waiting seq are answered from the buffer (not reprocessed).
  for (const auto& b : cp.buffer) {
    resp_buffer_[b.seq] = b.payload;
    req_hash_[b.seq] = b.hash;
  }
}

std::vector<Response> ColorServer::on_request(const Request& req) {
  std::vector<Response> out;

  // 0. Recovery: the client acknowledges history this (post-failover) server
  //    does not have. Ask it to replay the committed history we are missing.
  if (needs_recover(req)) {
    Response r;
    r.seq = req.seq;
    r.recover = true;
    r.recover_from = history_.event_count();
    out.push_back(r);
    return out;
  }

  // 1. Apply the acknowledgement: release responses the client has confirmed.
  release_acked(req);

  // 2. Exactly-once for an already-committed seq (seq <= committed_upto_).
  if (req.seq <= committed_upto_) {
    auto it = resp_buffer_.find(req.seq);
    if (it != resp_buffer_.end()) {
      // Committed, response still buffered (client waiting or ack lost): resend.
      out.push_back(make_response(req.seq, it->second, /*no_op=*/false));
    } else if (awaiting_.count(req.seq)) {
      // Replayed but never answered: (re)process exactly once now.
      std::string resp = app_(req.seq, req.payload, history_);
      resp_buffer_[req.seq] = resp;
      awaiting_.erase(req.seq);
      ++app_calls_[req.seq];
      out.push_back(make_response(req.seq, resp, /*no_op=*/false));
    } else {
      // Already acknowledged and released: acknowledgement-only reply.
      out.push_back(make_response(req.seq, "", /*no_op=*/true));
    }
    track_bounds();
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
    ++app_calls_[r.seq];  // must stay == 1 (exactly-once)

    std::string resp = app_(r.seq, r.payload, history_);
    resp_buffer_[r.seq] = resp;
    out.push_back(make_response(r.seq, resp, /*no_op=*/false));
  }

  track_bounds();
  return out;
}

Checkpoint ColorServer::checkpoint() const {
  Checkpoint cp;
  cp.committed_upto = committed_upto_;
  cp.event_count = history_.event_count();
  cp.history_hash = history_.cur_hash();
  for (const auto& kv : resp_buffer_) {
    Hash h = 0;
    auto hit = req_hash_.find(kv.first);
    if (hit != req_hash_.end()) h = hit->second;
    cp.buffer.push_back(BufferedResponse{kv.first, kv.second, h});
  }
  return cp;
}

void ColorServer::ingest_replay(const Replay& rp) {
  // Skip any prefix we already hold (idempotent replay).
  std::size_t have = history_.event_count();
  std::size_t skip = have > rp.from ? have - rp.from : 0;
  for (std::size_t i = skip; i < rp.events.size(); ++i) {
    const ReplayEvent& e = rp.events[i];
    if (e.is_response) {
      history_.append(Event{/*is_response=*/true, e.seq});
      awaiting_.erase(e.seq);     // the client has this response
      resp_buffer_.erase(e.seq);  // acked by the client; no need to retain
    } else {
      Hash h = history_.append(Event{/*is_response=*/false, e.seq});
      req_hash_[e.seq] = h;
      if (e.seq > committed_upto_) committed_upto_ = e.seq;
      // Tentatively missing its response; a following r<seq> clears this.
      awaiting_.insert(e.seq);
    }
  }
}

bool ColorServer::needs_recover(const Request& req) const {
  // The client's acknowledgement names responses we have not committed.
  if (req.ack_base > committed_upto_ + 1) return true;
  for (Seq i : req.ack_new)
    if (i > committed_upto_) return true;
  return false;
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
