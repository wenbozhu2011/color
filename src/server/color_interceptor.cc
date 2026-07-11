#include "color_interceptor.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "failover_json.h"
#include "net_http/public/response_code_enum.h"

namespace color {
namespace {

std::vector<Seq> parse_csv(absl::string_view s) {
  std::vector<Seq> out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = s.find(',', i);
    if (j == absl::string_view::npos) j = s.size();
    if (j > i) out.push_back(std::strtoull(std::string(s.substr(i, j - i)).c_str(),
                                           nullptr, 10));
    i = j + 1;
  }
  return out;
}

Seq parse_u64(absl::string_view s, Seq dflt) {
  if (s.empty()) return dflt;
  return std::strtoull(std::string(s).c_str(), nullptr, 10);
}

// Load and parse a checkpoint file, if present and valid.
bool load_checkpoint(const std::string& path, Checkpoint* out) {
  if (path.empty()) return false;
  std::ifstream f(path);
  if (!f.good()) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  return from_json_checkpoint(ss.str(), *out);
}

}  // namespace

ColorInterceptor::ColorInterceptor(ColorServer::AppFn app, bool set_hash,
                                   std::string checkpoint_path,
                                   std::size_t checkpoint_every)
    : set_hash_(set_hash),
      checkpoint_path_(std::move(checkpoint_path)),
      checkpoint_every_(checkpoint_every) {
  Checkpoint cp;
  if (load_checkpoint(checkpoint_path_, &cp)) {
    core_ = std::make_unique<ColorServer>(cp, std::move(app), set_hash);
    restored_ = true;
  } else {
    core_ = std::make_unique<ColorServer>(std::move(app), set_hash);
  }
}

std::string ColorInterceptor::ReadBody(net_http::ServerRequestInterface* req) {
  std::string body;
  int64_t size = 0;
  auto block = req->ReadRequestBytes(&size);
  if (block && size > 0) body.assign(block.get(), static_cast<std::size_t>(size));
  return body;
}

Request ColorInterceptor::Parse(net_http::ServerRequestInterface* req) {
  Request r;
  r.seq = parse_u64(req->GetRequestHeader("Color-Seq"), 0);
  r.ack_base = parse_u64(req->GetRequestHeader("Color-Ack-Base"), 1);
  r.ack_new = parse_csv(req->GetRequestHeader("Color-Ack-New"));
  absl::string_view h = req->GetRequestHeader("Color-Hash");
  if (!h.empty()) r.hash = parse_u64(h, 0);
  r.payload = ReadBody(req);
  return r;
}

void ColorInterceptor::Complete(net_http::ServerRequestInterface* req,
                                const Response& resp) {
  req->OverwriteResponseHeader("Color-Seq", std::to_string(resp.seq));
  if (set_hash_ && resp.hash)
    req->OverwriteResponseHeader("Color-Hash", std::to_string(*resp.hash));
  if (resp.no_op) req->OverwriteResponseHeader("Color-No-Op", "1");
  req->OverwriteResponseHeader("Content-Type", "application/json");
  req->WriteResponseString(resp.payload);
  req->ReplyWithStatus(net_http::HTTPStatusCode::OK);
}

net_http::InterceptResult ColorInterceptor::OnReplay(
    net_http::ServerRequestInterface* req) {
  std::string body = ReadBody(req);
  Replay rp;
  from_json_replay(body, rp);
  {
    std::lock_guard<std::mutex> lk(mu_);
    core_->ingest_replay(rp);
  }
  req->OverwriteResponseHeader("Content-Type", "application/json");
  req->ReplyWithStatus(net_http::HTTPStatusCode::OK);
  return net_http::InterceptResult::kExit;
}

net_http::InterceptResult ColorInterceptor::OnRequest(
    net_http::ServerRequestInterface* req) {
  // A replay request (recovery) carries Color-Replay and a JSON history body.
  if (!req->GetRequestHeader("Color-Replay").empty()) return OnReplay(req);

  Request creq = Parse(req);

  std::lock_guard<std::mutex> lk(mu_);
  // Hold this request's object so its reply can be produced when its seq
  // commits (now or when a later request fills the gap below it). If a stale
  // held object exists for the same seq (a superseded retry), abort it.
  auto old = pending_.find(creq.seq);
  if (old != pending_.end()) old->second->Abort();
  pending_[creq.seq] = req;

  auto responses = core_->on_request(creq);

  // Recovery signal: ask the client to replay from the given history position.
  if (responses.size() == 1 && responses[0].recover) {
    pending_.erase(creq.seq);
    req->OverwriteResponseHeader(
        "Color-Recover", "from=" + std::to_string(responses[0].recover_from));
    req->ReplyWithStatus(net_http::HTTPStatusCode::SERVICE_UNAV);
    return net_http::InterceptResult::kExit;
  }

  std::size_t committed = 0;
  for (const Response& resp : responses) {
    auto it = pending_.find(resp.seq);
    if (it == pending_.end()) continue;  // no held object (already replied)
    Complete(it->second, resp);
    pending_.erase(it);
    if (!resp.no_op) {
      ++committed;
      if (on_committed_) on_committed_(resp.seq, resp.payload);
    }
  }
  MaybeCheckpoint(committed);
  // Whether this request committed now or is held for later, Color owns the
  // response completion, so the handler must not run.
  return net_http::InterceptResult::kExit;
}

// Checkpoint sync semantics.
//
// Checkpointing is SYNCHRONOUS and happens on the request/executor thread while
// the conversation mutex (mu_) is held, every `checkpoint_every_` commits. So a
// checkpoint fully completes — snapshot serialized and durably renamed into
// place — before any subsequent request is processed. Two consequences:
//   - Because it runs under mu_, the snapshot is taken from a consistent core
//     state (no request can mutate the core mid-serialize).
//   - It briefly blocks request processing for the duration of the file write;
//     with a small `checkpoint_every_` the pauses are frequent but short.
// This is deliberately the simplest correct scheme. A production server would
// move the write off the hot path — snapshot under mu_, then serialize/fsync on
// a background or timer thread — trading this sync simplicity for lower latency.
void ColorInterceptor::MaybeCheckpoint(std::size_t committed) {
  if (checkpoint_path_.empty() || checkpoint_every_ == 0) return;
  commits_since_ckpt_ += committed;
  if (commits_since_ckpt_ >= checkpoint_every_) {
    WriteCheckpoint();
    commits_since_ckpt_ = 0;
  }
}

// Serialize the checkpoint and swap it into place atomically. Writing to a
// temp file and renaming means a crash mid-write cannot corrupt the live
// checkpoint: a replacement server always reads either the old or the new whole
// file, never a partial one. Caller holds mu_.
void ColorInterceptor::WriteCheckpoint() {
  std::string json = to_json(core_->checkpoint());
  std::string tmp = checkpoint_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << json;
  }
  std::rename(tmp.c_str(), checkpoint_path_.c_str());  // atomic replace
}

void ColorInterceptor::Register(net_http::HTTPServerInterface* server,
                                const std::string& uri) {
  server->RegisterRequestInterceptor(
      uri,
      [this](net_http::ServerRequestInterface* req) { return OnRequest(req); },
      /*response_interceptor=*/nullptr);
  // Fallback handler so the path is dispatchable; never reached (kExit above).
  server->RegisterRequestHandler(
      uri,
      [](net_http::ServerRequestInterface* req) { req->Abort(); },
      net_http::RequestHandlerOptions());
}

}  // namespace color
