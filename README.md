# Color

**Color is a protocol for stateful, ordered conversations between a client and a
server over plain HTTP `POST` requests.** It gives an ordinary stateless REST
endpoint the guarantees a conversation needs — strict ordering, exactly-once
processing, and a request/response history both sides agree on — without a
long-lived bidirectional stream, and it recovers seamlessly when the server
process fails over.

This repository contains the protocol design and a working C++17 prototype
(core library, verification harness, and a runnable client/server demo).

---

## 1. Core concept

Many services are *conversational*: every server message is a response caused by
a client request, and the order of those messages matters (think of a high-rate
chatbot). The obvious implementation — a bidirectional stream — is stateful,
which makes it harder to scale and load-balance, and hides the request/response
causality from the serving infrastructure.

Color keeps the wire **stateless** (only non-streamed `POST` RPCs, retried by an
ordinary HTTP client) while still delivering a well-defined, ordered conversation.
Its central guarantee is a single, identical total order of the whole
request/response history on both sides — *the server knows exactly what the
client has seen* — maintained even when requests and responses are arbitrarily
dropped, duplicated, or reordered.

→ Problem definition and properties: **[docs/spec.md](docs/spec.md)**

## 2. Implementation and proof

The protocol logic lives in a small, transport-agnostic **core library**
(`src/core/`). Its correctness is *proved* by a verification harness
(`verification/`) that runs the real core over a **simulated lossy network**
(seeded drop / duplicate / delay / reorder) across hundreds of randomized runs
and checks, on every run:

- **safety** — client and server agree on one identical history (cross-checked
  by a running history hash piggybacked on every message);
- **exactly-once** — each request is processed once despite retries;
- **liveness** — every request is eventually answered; and
- **bounded buffers** — retransmission state stays bounded, independent of run
  length.

The same suite injects **server failovers** and confirms the properties still
hold across them.

→ What is checked and how it is proved: **[docs/claude/verification.md](docs/claude/verification.md)**
(build & run: [verification/readme.md](verification/readme.md))

## 3. The basic REST protocol

A conversation is carried entirely in HTTP headers on ordinary `POST`s (the body
is your application payload; the URL/path is yours to choose):

**Request headers**

| Header | Meaning |
|---|---|
| `Color-Seq` | This request's id — contiguous and monotonic (`1, 2, 3, …`). |
| `Color-Ack-Base` | Low-water mark: all responses with id `< base` have been received. |
| `Color-Ack-New` | The responses received since the previous request, **in receipt order**. |
| `Color-Hash` *(optional)* | Verification only — a running hash of the client's history. |

**Response headers**: `Color-Seq` (echoes the request id) and, optionally,
`Color-Hash`.

**Client behavior:** assign the next `Color-Seq`, acknowledge what you've
received (`Color-Ack-Base` + `Color-Ack-New`), and retransmit the identical
request until a response arrives. That's it — a plain REST client with
transport-level retry is enough; no "rich client" required.

**Server behavior:** commit requests in id order, invoke the application exactly
once per request (a retried request returns the buffered response, never a
re-execution), and buffer each response until the client acknowledges it.

→ Full wire format, ordering rule, and the safety proof:
**[docs/claude/protocol.md](docs/claude/protocol.md)** ·
design rationale and decisions: **[docs/claude/plan.md](docs/claude/plan.md)**

## 4. Failover (extension to the basic protocol)

Failover is layered on top of the basic protocol and **does not change the
client's core algorithm** — the client keeps assigning ids, acknowledging, and
retransmitting exactly as above.

The server periodically **checkpoints** its active state to storage. A
replacement process (a restart on the same port, or a migration) loads the
checkpoint and resumes. Because a periodic checkpoint can lag the crash, the new
server may be missing recent history — in particular responses the client
already received. When it sees a request referencing history it lacks, it replies
`503` with a `Color-Recover` header; the client answers with a one-shot **replay**
of its known history (a `Color-Replay` request), the server rebuilds the exact
history, and the conversation continues — the application never notices.

→ Failover design and recovery protocol: **[docs/claude/failover.md](docs/claude/failover.md)**

## 5. Demo

The same core runs over real HTTP: a **libcurl** client and a **net_http**
server on one machine. The client injects request/response drops and retransmits;
with `--hash` on, both sides cross-check their history and a clean run reports
`hash mismatches=0`. **CMake only — no Bazel** (net_http and Abseil are fetched
automatically). This section is self-contained; it is also kept in
[demo/readme.md](demo/readme.md).

### Prerequisites (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y git cmake g++ pkg-config \
    libcurl4-openssl-dev libevent-dev zlib1g-dev
```

`git` is needed at configure time: CMake FetchContent pulls net_http (the
`server_interceptor` fork) and Abseil.

### Get the source

```sh
git clone https://github.com/wenbozhu2011/color.git
cd color
```

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # first run fetches net_http+abseil
cmake --build build -j
```

The programs land at `build/demo/src/color_client` and
`build/demo/src/color_server`. (`color_client` builds if libcurl is found;
`color_server` if libevent is found.)

### Run (same machine, two terminals)

```sh
# Terminal 1 — the server. One process == one conversation.
./build/demo/src/color_server --port 8080 --uri /color --hash

# Terminal 2 — the client, dropping ~30% of requests and responses.
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 30 --interval-ms 1000 --drop 0.3 --drop-resp 0.3 --hash
```

The server prints a `[commit]` line per request as it commits in order; the
client prints each delivered reply and every retransmitted attempt. Despite the
drops, the conversation advances in order and finishes with `hash mismatches=0`.

Try more concurrency (exercises out-of-order arrival and the interceptor's
in-order commit):

```sh
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 40 --parallel 4 --interval-ms 20 --drop 0.4 --drop-resp 0.4 --hash
```

### Failover demo (kill and restart the server)

Give the server a checkpoint file and it will periodically persist its state and
restore from it on startup. Kill the server mid-conversation and restart it on
the same port — the client keeps going, unaware.

```sh
# Terminal 1 — server with a checkpoint file, checkpointing every 5 commits.
./build/demo/src/color_server --port 8080 --uri /color --hash \
    --checkpoint /tmp/color.ckpt --checkpoint-every 5

# Terminal 2 — a longer client run so you have time to kill the server.
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 100 --interval-ms 200 --drop 0.2 --drop-resp 0.2 --hash

# Terminal 1 — Ctrl-C (or `kill -9`) the server, then restart it, same command:
./build/demo/src/color_server --port 8080 --uri /color --hash \
    --checkpoint /tmp/color.ckpt --checkpoint-every 5
```

On restart the server prints `restored from checkpoint … (committed_upto=N)`.
The client prints a single `[recover] … replayed K history events` line as it
replays the small tail the checkpoint lagged, then the conversation continues
and still finishes with `hash mismatches=0`. The client command line is
identical to the non-failover demo — nothing about running the client changes.

### Options

**color_server** — `--port` (8080), `--threads` (4), `--uri` (/color),
`--hash`, `--quiet`, `--checkpoint FILE`, `--checkpoint-every N` (16).

**color_client** — `--url`, `--count` (20), `--interval-ms` (1000),
`--parallel` (1), `--drop` (0.3), `--drop-resp` (0.3), `--seed` (1), `--hash`,
`--quiet`. Failover recovery (the `503`/replay handshake) is automatic; there is
no client flag for it.

> **One server process is one conversation.** A fresh client restarts its
> sequence at 1, so pointing a new client at an already-used server makes its
> early requests look like duplicate retries of the previous conversation
> (you'll see hash mismatches). Start a fresh server per client run — but a
> *restarted* server that reloads its checkpoint continues the same
> conversation, which is exactly the failover case above.
