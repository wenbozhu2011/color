# Color real transport — client & server

The same Color core (`src/core/`) that the verification harness exercises over a
simulated network also runs over real HTTP:

- **`src/client/`** — a libcurl-based client. A thin wrapper injects delivery
  failures (drops the request or the response); ordinary transport-level retry
  re-POSTs the identical request until a reply arrives.
- **`src/server/`** — Color as a **net_http request interceptor**: a reusable,
  framework-level library that makes a plain echo endpoint speak Color,
  transparent to the application handler.

Both are intended to run on the same machine (loopback).

## Client (builds with just libcurl)

```sh
sudo apt-get install -y libcurl4-openssl-dev      # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/src/client/color_client --url http://127.0.0.1:8080/color \
    --count 20 --interval-ms 1000 --drop 0.3 --drop-resp 0.3
```

Client options: `--url`, `--count`, `--interval-ms`, `--parallel`,
`--drop` (request-drop prob), `--drop-resp` (response-drop prob), `--seed`,
`--hash` (send the optional verification hash), `--quiet`.

## Server (requires net_http)

`net_http` (`wenbozhu2011/net_http`, branch `server_interceptor`) is a Bazel
project and pulls in abseil + libevent, so it is not built by this repo's CMake.
Build/stage it, then point this build at it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DNET_HTTP_ROOT=/path/to/net_http \
    -DNET_HTTP_INCLUDE_DIRS="/path/to/abseil/include" \
    -DNET_HTTP_LIBS="/path/to/libnet_http.a;/path/to/libabsl_strings.a;event;pthread"
cmake --build build -j
./build/src/server/color_server --port 8080 --uri /color --threads 4
```

- `NET_HTTP_ROOT` — directory that contains the `net_http/` header tree (so
  `#include "net_http/server/public/httpserver.h"` resolves).
- `NET_HTTP_INCLUDE_DIRS` — extra include dirs (abseil, …).
- `NET_HTTP_LIBS` — the link libraries (net_http, the abseil libs it uses,
  `event`, `pthread`).

When `NET_HTTP_ROOT` is unset, the server target is skipped and the rest of the
repo still builds.

Server options: `--port`, `--threads`, `--uri`, `--hash`, `--quiet`.

## Run them together (same VM)

```sh
./build/src/server/color_server --port 8080 &          # terminal 1
./build/src/client/color_client --url http://127.0.0.1:8080/color \
    --count 30 --interval-ms 1000 --drop 0.3 --drop-resp 0.3   # terminal 2
```

Watch the client retransmit through dropped messages while the conversation
still advances in order. For the failover demo, kill and restart the server on
the same port — the client keeps retrying, unaware.
