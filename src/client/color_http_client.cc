#include "color_http_client.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "color_json.h"

namespace color {

ColorHttpClient::ColorHttpClient(std::string url, CurlTransport* transport,
                                 DeliverFn on_deliver, bool set_hash)
    : url_(std::move(url)),
      transport_(transport),
      core_(std::move(on_deliver), set_hash) {}

std::vector<std::string> ColorHttpClient::header_lines(const Request& r) {
  std::vector<std::string> h;
  h.push_back("Color-Seq: " + std::to_string(r.seq));
  h.push_back("Color-Ack-Base: " + std::to_string(r.ack_base));
  std::string acks;
  for (std::size_t i = 0; i < r.ack_new.size(); ++i) {
    if (i) acks += ',';
    acks += std::to_string(r.ack_new[i]);
  }
  h.push_back("Color-Ack-New: " + acks);
  if (r.hash) h.push_back("Color-Hash: " + std::to_string(*r.hash));
  h.push_back("Content-Type: application/json");
  return h;
}

void ColorHttpClient::send(const std::string& payload, int retry_delay_ms) {
  Request req;
  {
    std::lock_guard<std::mutex> lk(mu_);
    req = core_.generate_request(payload);
  }
  const std::vector<std::string> headers = header_lines(req);

  for (int attempt = 1;; ++attempt) {
    Injected inj = Injected::kNone;
    HttpResult res = transport_->post(url_, headers, req.payload, &inj);

    // Failover recovery: the server lost history and asks us to replay. Handle
    // it, then re-POST the same request. The core conversation is unchanged.
    if (res.delivered && res.status == 503) {
      auto rit = res.headers.find("color-recover");
      if (rit != res.headers.end()) {
        const char* v = rit->second.c_str();
        const char* eq = std::strchr(v, '=');
        recover(std::strtoull(eq ? eq + 1 : v, nullptr, 10));
        continue;  // resend the original request to the rebuilt server
      }
    }

    if (on_attempt_) on_attempt_({req.seq, inj, res.delivered, attempt});

    if (res.delivered) {
      Response resp;
      resp.seq = req.seq;  // the reply is for this request
      auto sit = res.headers.find("color-seq");
      if (sit != res.headers.end()) resp.seq = std::strtoull(sit->second.c_str(), nullptr, 10);
      auto hit = res.headers.find("color-hash");
      if (hit != res.headers.end()) resp.hash = std::strtoull(hit->second.c_str(), nullptr, 10);
      resp.no_op = res.headers.count("color-no-op") != 0;
      resp.payload = res.body;
      {
        std::lock_guard<std::mutex> lk(mu_);
        core_.on_response(resp);
      }
      return;
    }
    // Dropped or errored: pace, then re-POST the identical (frozen) request.
    std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
  }
}

void ColorHttpClient::recover(std::size_t from) {
  std::string body;
  std::size_t nevents = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    Replay rp = core_.build_replay(from);
    nevents = rp.events.size();
    body = to_json(rp);
  }
  const std::vector<std::string> headers = {
      "Color-Replay: " + std::to_string(from), "Content-Type: application/json"};
  // POST the replay, retrying on drops until the server acknowledges it.
  for (;;) {
    Injected inj = Injected::kNone;
    HttpResult res = transport_->post(url_, headers, body, &inj);
    if (res.delivered && res.status == 200) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (on_recover_) on_recover_(from, nevents);
}

}  // namespace color
