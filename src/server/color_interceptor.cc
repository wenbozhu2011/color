#include "color_interceptor.h"

#include <cstdlib>
#include <vector>

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

}  // namespace

ColorInterceptor::ColorInterceptor(ColorServer::AppFn app, bool set_hash)
    : core_(std::move(app), set_hash), set_hash_(set_hash) {}

Request ColorInterceptor::Parse(net_http::ServerRequestInterface* req) {
  Request r;
  r.seq = parse_u64(req->GetRequestHeader("Color-Seq"), 0);
  r.ack_base = parse_u64(req->GetRequestHeader("Color-Ack-Base"), 1);
  r.ack_new = parse_csv(req->GetRequestHeader("Color-Ack-New"));
  absl::string_view h = req->GetRequestHeader("Color-Hash");
  if (!h.empty()) r.hash = parse_u64(h, 0);

  int64_t size = 0;
  auto block = req->ReadRequestBytes(&size);
  if (block && size > 0) r.payload.assign(block.get(), static_cast<std::size_t>(size));
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

net_http::InterceptResult ColorInterceptor::OnRequest(
    net_http::ServerRequestInterface* req) {
  Request creq = Parse(req);

  std::lock_guard<std::mutex> lk(mu_);
  // Hold this request's object so its reply can be produced when its seq
  // commits (now or when a later request fills the gap below it). If a stale
  // held object exists for the same seq (a superseded retry), abort it.
  auto old = pending_.find(creq.seq);
  if (old != pending_.end()) old->second->Abort();
  pending_[creq.seq] = req;

  for (const Response& resp : core_.on_request(creq)) {
    auto it = pending_.find(resp.seq);
    if (it == pending_.end()) continue;  // no held object (already replied)
    Complete(it->second, resp);
    pending_.erase(it);
    if (on_committed_ && !resp.no_op) on_committed_(resp.seq, resp.payload);
  }
  // Whether this request committed now or is held for later, Color owns the
  // response completion, so the handler must not run.
  return net_http::InterceptResult::kExit;
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
