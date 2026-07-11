#include "color_http_client.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace color {

ColorHttpClient::ColorHttpClient(std::string url, FaultHttpTransport* transport,
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

}  // namespace color
