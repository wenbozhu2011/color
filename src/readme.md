# Color libraries (`src/`)

The transport-agnostic Color core and the two real-HTTP transport libraries that
build on it. These are libraries only — the runnable client/server programs live
in `demo/` (see `demo/readme.md` to build and run them).

```
src/core/     color_core         — transport-agnostic protocol core
src/client/   color_http_client  — libcurl client transport (CurlTransport)
src/server/   color_interceptor  — Color as a net_http request interceptor
```

## What builds when

The core always builds. Each transport library builds only when its
dependencies are present (all handled by the top-level CMake):

| Library | Needs | Notes |
|---|---|---|
| `color_core` | C++17 compiler | always built |
| `color_http_client` | libcurl | built when `find_package(CURL)` succeeds |
| `color_interceptor` | libevent + zlib | net_http + abseil are fetched via FetchContent; skip with `-DCOLOR_BUILD_SERVER=OFF` |

## Prerequisites (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y git cmake g++ pkg-config \
    libcurl4-openssl-dev libevent-dev zlib1g-dev
```

`git` is needed at configure time — FetchContent pulls net_http (the
`server_interceptor` fork) and Abseil.

## Build the libraries

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # first run fetches net_http+abseil
cmake --build build --target color_core color_http_client color_interceptor -j
```

Link against them from your own code as `color_core`, `color_http_client`, or
`color_interceptor`; each carries its public include directory and transitive
dependencies. The verification harness (`verification/`) links `color_core`; the
demo programs (`demo/`) link `color_http_client` and `color_interceptor`.
