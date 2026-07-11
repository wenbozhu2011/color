// Color server program: a net_http HTTP server whose Color interceptor makes a
// plain echo endpoint speak the Color conversation protocol.
//
// The application is a trivial echo (replies with the server timestamp), fully
// unaware of Color. With --checkpoint, the server periodically saves its state
// to a JSON file and restores from it on startup: kill and restart this process
// on the same port (same --checkpoint) to exercise the failover demo; the client
// keeps going, oblivious.
//
// Usage:
//   color_server [--port P] [--threads T] [--uri U] [--hash] [--quiet]
//                [--checkpoint FILE] [--checkpoint-every N]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "color_history.h"
#include "color_interceptor.h"
#include "color_server.h"
#include "net_http/server/public/httpserver.h"
#include "net_http/server/public/httpserver_interface.h"
#include "thread_pool_executor.h"

namespace {

long now_ms() {
  auto d = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer so redirected logs flush
  int port = 8080;
  int threads = 4;
  std::string uri = "/color";
  bool set_hash = false;
  bool quiet = false;
  std::string checkpoint;
  int checkpoint_every = 16;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](int& i) { return std::string(argv[++i]); };
    if (a == "--port") port = std::stoi(next(i));
    else if (a == "--threads") threads = std::stoi(next(i));
    else if (a == "--uri") uri = next(i);
    else if (a == "--hash") set_hash = true;
    else if (a == "--quiet") quiet = true;
    else if (a == "--checkpoint") checkpoint = next(i);
    else if (a == "--checkpoint-every") checkpoint_every = std::stoi(next(i));
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
  }

  auto options = std::make_unique<net_http::ServerOptions>();
  options->AddPort(port);
  options->SetExecutor(std::make_unique<color::ThreadPoolExecutor>(threads));

  auto server = net_http::CreateEvHTTPServer(std::move(options));
  if (server == nullptr) {
    std::fprintf(stderr, "failed to create server on port %d\n", port);
    return 1;
  }

  // The application: a Color-unaware echo returning the server timestamp.
  color::ColorServer::AppFn app = [](color::Seq seq, const std::string& /*payload*/,
                                     const color::History&) {
    return "{\"srv_ts\":" + std::to_string(now_ms()) +
           ",\"seq\":" + std::to_string(seq) + "}";
  };

  color::ColorInterceptor color(app, set_hash, checkpoint,
                                static_cast<std::size_t>(checkpoint_every));
  if (color.restored())
    std::printf("restored from checkpoint %s (committed_upto=%llu)\n",
                checkpoint.c_str(), (unsigned long long)color.committed_upto());
  if (!quiet) {
    color.on_committed([](color::Seq seq, const std::string& payload) {
      std::printf("  [commit] seq=%llu -> %s\n", (unsigned long long)seq,
                  payload.c_str());
    });
  }
  color.on_hash_mismatch([](color::Seq seq, color::Hash got, color::Hash exp) {
    std::printf("  [HASH MISMATCH] seq=%llu got=%llu expected=%llu\n",
                (unsigned long long)seq, (unsigned long long)got,
                (unsigned long long)exp);
  });
  color.Register(server.get(), uri);

  if (!server->StartAcceptingRequests()) {
    std::fprintf(stderr, "failed to start accepting requests\n");
    return 1;
  }
  std::printf("color_server listening on port %d, uri %s (threads=%d)\n",
              server->listen_port(), uri.c_str(), threads);
  server->WaitForTermination();
  return 0;
}
