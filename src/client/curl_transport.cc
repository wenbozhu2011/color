#include "curl_transport.h"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <random>
#include <thread>

namespace color {
namespace {

// Per-thread RNG so concurrent senders inject independently and reproducibly.
std::mt19937_64& thread_rng(std::uint64_t seed) {
  static std::atomic<std::uint64_t> counter{0};
  thread_local std::mt19937_64 rng(seed ^ (0x9e3779b97f4a7c15ULL *
                                           (counter.fetch_add(1) + 1)));
  return rng;
}

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb,
                       void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::size_t write_header(char* ptr, std::size_t size, std::size_t nmemb,
                         void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  std::size_t n = size * nmemb;
  std::string line(ptr, n);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    for (char& c : key) c = static_cast<char>(std::tolower((unsigned char)c));
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
      std::size_t i = 0;
      while (i < s.size() && s[i] == ' ') ++i;
      s.erase(0, i);
    };
    trim(key);
    trim(val);
    (*headers)[key] = val;
  }
  return n;
}

}  // namespace

CurlTransport::CurlTransport(double p_drop_request, double p_drop_response,
                             std::uint64_t seed, long timeout_ms)
    : p_drop_request_(p_drop_request),
      p_drop_response_(p_drop_response),
      seed_(seed),
      timeout_ms_(timeout_ms) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlTransport::~CurlTransport() { curl_global_cleanup(); }

Injected CurlTransport::roll() {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  auto& rng = thread_rng(seed_);
  if (u(rng) < p_drop_request_) return Injected::kDropRequest;
  if (u(rng) < p_drop_response_) return Injected::kDropResponse;
  return Injected::kNone;
}

HttpResult CurlTransport::post(const std::string& url,
                                    const std::vector<std::string>& header_lines,
                                    const std::string& body, Injected* injected) {
  HttpResult res;
  Injected inj = roll();
  if (injected) *injected = inj;

  // Drop the outgoing request: the server never sees it.
  if (inj == Injected::kDropRequest) return res;

  CURL* h = curl_easy_init();
  if (!h) return res;

  struct curl_slist* slist = nullptr;
  for (const auto& line : header_lines) slist = curl_slist_append(slist, line.c_str());
  // Prevent libcurl from adding an Expect: 100-continue round trip.
  slist = curl_slist_append(slist, "Expect:");

  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, slist);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &res.headers);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms_);

  CURLcode rc = curl_easy_perform(h);
  if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);

  curl_slist_free_all(slist);
  curl_easy_cleanup(h);

  if (rc != CURLE_OK) return res;  // transport error == not delivered; retry

  // Drop the incoming response: the server processed it, but the client
  // discards the reply and will retry (exercising exactly-once recovery).
  if (inj == Injected::kDropResponse) {
    res.body.clear();
    res.headers.clear();
    res.status = 0;
    return res;
  }

  res.delivered = true;
  return res;
}

}  // namespace color
