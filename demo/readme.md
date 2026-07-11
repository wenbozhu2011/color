# Color demo

Runnable programs that drive a real Color conversation over HTTP (libcurl client
↔ net_http server) on one machine. The client injects request/response drops and
retransmits; with `--hash` on, both sides cross-check their history and a clean
run reports `hash mismatches=0`.

```
demo/src/color_client_main.cc  -> color_client   (libcurl, links color_http_client)
demo/src/color_server_main.cc  -> color_server   (net_http, links color_interceptor)
```

## Prerequisites (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y git cmake g++ pkg-config \
    libcurl4-openssl-dev libevent-dev zlib1g-dev
```

`git` is needed at configure time: CMake FetchContent pulls net_http (the
`server_interceptor` fork) and Abseil. **CMake only — no Bazel.**

## Get the source

```sh
git clone https://github.com/wenbozhu2011/color.git
cd color
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # first run fetches net_http+abseil
cmake --build build -j
```

The programs land at `build/demo/src/color_client` and
`build/demo/src/color_server`. (`color_client` builds if libcurl is found;
`color_server` if libevent is found.)

## Run (same machine, two terminals)

```sh
# Terminal 1 — the server. One process == one conversation.
./build/demo/src/color_server --port 8080 --uri /color --hash

# Terminal 2 — the client, dropping ~30% of requests and responses.
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 30 --interval-ms 1000 --drop 0.3 --drop-resp 0.3 --hash
```

The server prints a `[commit]` line per request as it commits in order; the
client prints each delivered reply and every retransmitted attempt. Despite the
drops, the conversation advances in order and finishes with
`hash mismatches=0`.

Try more concurrency (exercises out-of-order arrival and the interceptor's
in-order commit):

```sh
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 40 --parallel 4 --interval-ms 20 --drop 0.4 --drop-resp 0.4 --hash
```

## Options

**color_server** — `--port` (8080), `--threads` (4), `--uri` (/color),
`--hash`, `--quiet`.

**color_client** — `--url`, `--count` (20), `--interval-ms` (1000),
`--parallel` (1), `--drop` (0.3), `--drop-resp` (0.3), `--seed` (1), `--hash`,
`--quiet`.

## Notes

- **One server process is one conversation.** A fresh client restarts its
  sequence at 1, so pointing a new client at an already-used server makes its
  early requests look like duplicate retries of the previous conversation
  (you'll see hash mismatches). Start a fresh server per client run.
- **Failover** (kill and restart the *server* on the same port while the client
  keeps going) needs the Phase II checkpoint/reload support and will be added
  with that milestone; it is not part of this Phase I demo.
