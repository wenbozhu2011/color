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
with hashing on, both sides continuously cross-check their histories. For the
failover demo you literally **kill the server and restart it on the same port** —
the client keeps going, replays the small lagged tail, and the "chat" continues
with zero history mismatches.

```sh
# build (CMake only; fetches net_http + abseil on first configure)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# terminal 1: server
./build/demo/src/color_server --port 8080 --hash --checkpoint /tmp/color.ckpt

# terminal 2: client
./build/demo/src/color_client --url http://127.0.0.1:8080/color \
    --count 100 --drop 0.2 --drop-resp 0.2 --hash
```

→ Prerequisites, full walkthrough, and the failover steps:
**[demo/readme.md](demo/readme.md)**
