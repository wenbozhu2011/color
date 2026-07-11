# Color real transport — client & server

The same Color core (`src/core/`) that the verification harness exercises over a
simulated network also runs over real HTTP:

- **`src/client/`** — a libcurl-based client. A thin wrapper injects delivery
  failures (drops the request or the response); ordinary transport-level retry
  re-POSTs the identical request until a reply arrives.
- **`src/server/`** — Color as a **net_http request interceptor**: a reusable,
  framework-level library that makes a plain echo endpoint speak Color,
  transparent to the application handler.

Both run on the same machine (loopback). **CMake only** — `net_http` and Abseil
are fetched and compiled automatically (`FetchContent`); no Bazel needed.

## Prerequisites (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y git cmake g++ pkg-config \
    libcurl4-openssl-dev libevent-dev zlib1g-dev
```

- `libcurl` → the client. `libevent` + `zlib` → the net_http server backend.
- `git` is needed at configure time: FetchContent pulls net_http and Abseil.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # first run fetches net_http+abseil
cmake --build build -j
```

This builds `color_client` (if libcurl is found) and `color_server` (if libevent
is found). To skip the server: `-DCOLOR_BUILD_SERVER=OFF`.

## Run (same VM)

```sh
# Terminal 1 — the server (one process == one conversation).
./build/src/server/color_server --port 8080 --uri /color --hash

# Terminal 2 — the client, injecting drops on both directions.
./build/src/client/color_client --url http://127.0.0.1:8080/color \
    --count 30 --interval-ms 1000 --drop 0.3 --drop-resp 0.3 --hash
```

Watch the client retransmit through dropped messages while the conversation
still advances in order. With `--hash` on both sides, every reply's history hash
is cross-checked; a clean run reports `hash mismatches=0`.

> One server process is one conversation. Point a fresh client (seq restarts at
> 1) at an already-used server and its early requests look like duplicate
> retries of the old conversation. For the Phase II failover demo you restart
> the *server* (which reloads its checkpoint), never the client.

### Options

Client: `--url`, `--count`, `--interval-ms`, `--parallel`, `--drop`,
`--drop-resp`, `--seed`, `--hash`, `--quiet`.
Server: `--port`, `--threads`, `--uri`, `--hash`, `--quiet`.
