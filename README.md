# Color

**Color refers to a general spec for REST-based conversational communication between 
a client and server.** It gives an ordinary stateless REST/HTTP
endpoint the guarantees that a bidi conversation needs without exposing any stateful endpoint.
Color also enables seamless server failover.

This repository contains the protocol design and a working C++ prototype
(core library as a framework, verification & proof, and a runnable client/server demo).

---

## 1. Core concept

Many services are *conversational*: every server message is a response caused by
a client request, and the order of those messages matters (think of a high-rate
chatbot). The obvious implementation — a bidi stream — is stateful,
which makes it harder to scale and load-balance, and hides the request/response
causality from the serving infrastructure.

Color keeps the endpoint **stateless** (only non-streamed `POST` RPCs, retried by an
ordinary HTTP client) while still delivering well-defined semantics for the conversation
even when multiple requests and responses are generated concurrently and they may
be arbitrarily dropped, duplicated, or reordered.

→ Communication properties: **[docs/spec.md](docs/spec.md)**

## 2. Implementation and proof

The protocol logic lives in a small, transport-agnostic **core framework library**
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

→ See **[docs/claude/verification.md](docs/claude/verification.md)**
(build & run: [verification/readme.md](verification/readme.md))

## 3. The basic REST protocol

A conversation is carried entirely in HTTP headers on ordinary `POST`s:

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
request until a response arrives. That's it — a regular REST client with
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

The server periodically **checkpoints** the conversation history, which is available
to a new server process when a failover is triggered. Because a periodic checkpoint can lag the crash, the new
server may be missing recent history — in particular responses the client
already received. When it sees a request referencing history it lacks, it replies
`503` with a `Color-Recover` header; the client answers with a one-shot **replay**
of its known history (a `Color-Replay` request), the server rebuilds the exact
history, and the conversation continues — the application never notices.

→ See: **[docs/claude/failover.md](docs/claude/failover.md)**

## 5. Demo

The same core runs over real HTTP: a **libcurl** client and a **google/net_http**
server on one machine. The client injects request/response drops and retransmits;
with `--hash` on, both sides cross-check their history and a clean run reports
`hash mismatches=0`. More details: 
[demo/readme.md](demo/readme.md).

### Prerequisites (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y git cmake g++ pkg-config \
    libcurl4-openssl-dev libevent-dev zlib1g-dev
```

### Get the source

```sh
git clone https://github.com/wenbozhu2011/color.git
cd color
```

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Run (same machine)

```sh
# Terminal 1 — the server. One process == one conversation.
./build/demo/src/color_server --port 8080 --uri /color --hash

# Terminal 2 — the client, dropping ~30% of requests and responses.
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 20 --interval-ms 1000 --drop 0.3 --drop-resp 0.3 --hash
```

The server prints a `[commit]` line per request as it commits in order; the
client prints each delivered reply and every retransmitted attempt. Despite the
drops, the conversation advances in order and finishes with `hash mismatches=0`.

More concurrency and out-of-order arrivals:

```sh
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 40 --parallel 4 --interval-ms 20 --drop 0.4 --drop-resp 0.4 --hash
```

### Failover demo

Give the server a checkpoint file and it will periodically persist its state and
restore from it on startup. Kill the server mid-conversation and restart it on
the same port — the client keeps going.

```sh
# Terminal 1 — server with a checkpoint file, checkpointing every 5 commits.
./build/demo/src/color_server --port 8080 --uri /color --hash \
    --checkpoint /tmp/color.ckpt --checkpoint-every 5

# Terminal 2 — a longer client run
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 100 --interval-ms 200 --drop 0.2 --drop-resp 0.2 --hash

# Terminal 1 — Ctrl-C the server, then restart it, same command:
./build/demo/src/color_server --port 8080 --uri /color --hash \
    --checkpoint /tmp/color.ckpt --checkpoint-every 5
```

On restart the server prints `restored from checkpoint … (committed_upto=N)`.
The client prints a single `[recover] … replayed K history events` line as it
replays the small tail the checkpoint lagged, then the conversation continues
and still finishes with `hash mismatches=0`. 

