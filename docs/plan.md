# Color — Project Plan and Shared Understanding

Status: **DRAFT for review**. This document summarizes my understanding of the
problem (`spec.md`) and the implementation requirements (`requirements.md`)
*before* we finalize the protocol design (`docs/protocol.md`) and start coding.
It is intentionally a restatement + open-questions document, not the design
itself. Please correct anything below that misrepresents the intent.

---

## 1. What Color is (problem definition, from `spec.md`)

**Color is a REST/stateless-RPC protocol that supports a stateful, ordered
"conversation" between a single client and a single logical server endpoint.**

The motivating example is a high-rate conversational service (e.g. a
"high-hz chatbot"): every server message is a *response* caused by a client
*request* — there are no spontaneous, one-way messages in either direction.

Why not just use a bidirectional stream?
- A bidi stream is a *stateful* API, which is less reliable/scalable than
  stateless RPC and tends to become an opaque tunnel.
- The serving infra (frameworks, proxies) can't see the request/response
  causality inside a stream, so applications end up re-annotating messages
  anyway.

So Color expresses the conversation using **only non-streamed POST RPCs**,
while still giving the application a well-defined, ordered message history.

### The guarantees Color must provide (my reading of §"Communication properties")

**Safety**
- S1. **Ordered requests.** The client emits requests in a significant order;
  that order is meaningful to both sides.
- S2. **Every request eventually gets exactly one response** (including
  app-level errors). The client may retry a request until a response arrives.
- S3. **Response receipt is symmetric.** When the client has *received* a
  response, the server *knows* the client received it. (No response is
  "delivered" from the app's view without the server being able to confirm it.)
- S4. **Causal acknowledgement.** When the client generates a new request, all
  responses it already knows about "happen before" that request; that ordering
  of prior responses is also significant.
- S5. **Exactly-once processing.** A request is processed on the server at most
  once. A retried request must return the *buffered* prior response, never be
  re-executed to reconstruct a lost response.
- **Overall safety invariant (the thing we must prove):** the client and the
  server agree on **one identical total order** of the request/response history
  — "the server knows exactly what the client sees."

**Liveness**
- L1. **Progress under loss.** Requests and responses may be dropped
  arbitrarily and independently; the client retries immediately. Despite this,
  the conversation makes progress.
- L2. **Bounded buffers.** The server buffers each response until the client
  has received it; retransmission-buffer growth must stay bounded.

**Concurrency**
- Parallel in-flight requests are allowed (multiple responses pending at once),
  and their responses may arrive in a different order than the requests were
  sent. Hence **response ids can have gaps** relative to what the client has
  seen at any instant.

### Non-goals / simplifying assumptions (Phase I)
- No server failover (that's Phase II).
- No request/response batching (except the dedicated history-replay request
  used by failover recovery).
- No "rich client" required: a plain REST client + transport-level retry must be
  able to speak Color. Retries are handled *below* Color (HTTP client retry),
  not inside the app.

---

## 2. Protocol mechanism (high-level sketch — details go in `protocol.md`)

From `requirements.md` §"REST protocol design", the wire mechanism rests on
three header ideas. I'll flesh these out in `protocol.md`; here I just record
the intent so we agree on the skeleton:

1. **Request id** — every request carries a unique id (header); the response
   echoes it back. This is the dedup/idempotency key for exactly-once (S5).
2. **Client-known-responses acknowledgement** — every request carries a header
   describing *which responses the client has already received* at the moment
   the request is generated. Because responses can arrive with gaps
   (concurrency), this is not a single high-water mark; it must be able to
   express a non-contiguous set (a "SACK-like" acknowledgement). This header is
   what:
   - pins each new request *after* the responses it acknowledges (S4), and
   - tells the server which buffered responses are safe to release (L2), and
   - lets the server confirm the client has seen a response (S3).
3. **Gap-tolerant response ids** — responses carry ids that may be
   non-contiguous relative to what the client currently holds; the protocol
   must define how the client detects and tolerates gaps.

**The key thing to prove in `protocol.md`:** these headers induce a
happens-before relation (request → its response; acknowledged responses → the
acknowledging request) whose linearization is *identical* on both sides. That's
the S-invariant.

### Open design questions to settle in `protocol.md`
- **Q1. Acknowledgement encoding.** How does the client express "responses I've
  seen" compactly given gaps? (Options: highest-contiguous id + explicit list of
  received ids beyond the gap; a bitmap window; run-length of ranges.) This
  drives header size and buffer-release logic.
- **Q2. Id spaces.** One monotonic id space assigned by the client per request,
  with the response reusing the request's id? Or separate request-id and
  response-id spaces? (Requirements say "responses may have gaps *w.r.t. the
  request ids*", which suggests responses are keyed by their request id and the
  gaps come from out-of-order completion.) Need to state this precisely.
- **Q3. Buffer-release rule.** Exact condition under which the server may drop a
  buffered response, and under which the client may stop expecting a response —
  and the argument that both stay bounded under continued progress.
- **Q4. History hashing for verification.** `requirements.md` §verification
  suggests hashing the ordered request/response history (keyed by last
  request/response id) and carrying the hash on each message so each side can
  check its history against the peer's. Need to define exactly what is hashed
  and how the two sides align on the same prefix before comparing.
- **Q5. Total-order definition.** State the canonical ordering rule that both
  sides independently compute (e.g. order by request id, with each response
  slotted immediately after the request that acknowledges it / after its own
  request), and prove both sides derive the same sequence.

---

## 3. Deliverables and repository layout (from `requirements.md`)

Requirements reference paths under `color/...`; since this repo *is* `color`,
I read those as repo-root paths. Proposed layout:

```
docs/
  spec.md            # problem definition (exists)
  requirements.md    # implementation requirements (exists)
  plan.md            # THIS document
  protocol.md        # Phase I REST protocol design (to write)
  failover.md        # Phase II failover protocol design (to write)
client/
  plan.md            # client implementation plan
  src/               # libcurl-based client + failure-injection wrapper
server/
  plan.md            # server framework implementation plan
  src/               # Color as a net_http interceptor-based C++ library
verification/
  plan.md            # verification harness plan
  src/               # fuzzy client/server driver + correctness checker
demo/
  plan.md            # demo plan
  src/               # slowed-down, event-printing demo built on the fuzzer
  readme.md          # install/build/run instructions (cmake only)
```

(If you'd prefer everything nested under a `color/` subdirectory instead of
repo-root, tell me and I'll adjust.)

---

## 4. Component requirements (from `requirements.md`)

### Client
- libcurl as the HTTP client.
- A thin C++ wrapper over libcurl **only** to inject failures (randomly drop the
  request or the response).
- Injected failures trigger an *immediate* HTTP retry, handled **outside** the
  Color logic (transport-level retry).

### Server (framework)
- Built on **`wenbozhu2011/net_http`** (fork of `google/net_http`),
  multi-threaded, using the **interceptor API on the `server_interceptor`
  branch**.
- Color implemented as a **reusable, framework-level C++ library** (an
  interceptor), transparent to RPC application logic.
- The app sees the ordered request/response history ("conversation state").
- Verification/demo relies on **client-side** failure injection only.

### Verification
- Prove Color guarantees an identical total-ordered request/response history on
  both sides.
- A fuzzy client/server driver: client request = timestamp (+ optional random
  nonce); server reply = its own timestamp payload.
- Requests/responses dropped or duplicated randomly.
- Check the `spec.md` correctness properties; compare per-side history via the
  hash-carried-on-each-message scheme (Q4).
- Run up to ~5 min co-located; optionally inject server processing delay.

### Demo
- Reuse the fuzzer as the demo, but slow the message rate, run continuously, and
  print client/server events to make the protocol legible.
- `readme.md` with full install/build/run instructions; **cmake only**.

### Phase II — Failover
- Protocol design in `docs/failover.md`, including the **logical data structure
  for checkpointing** the request/response history on the server.
- Persist history to a local JSON file, periodically.
- Server quits/restarts (possibly simulated); the new server reads the history.
- Client keeps retrying, unaware of the restart/failover.
- On failover, the server may fail a request with a special **5xx** so the
  client re-sends its known requests/responses (a dedicated history-replay
  request with a JSON envelope); re-sent requests get reprocessed by the new
  server.
- Re-run the same verification after failover.
- Demo: kill the server, restart on the same port, observe the "chat" continue.

---

## 5. Dependency & environment note — needs your call

There is a tension between the requirements and what actually builds/runs in
this session, and I want to surface it before coding:

- **As specified**, the client depends on **libcurl** and the server on the
  **`wenbozhu2011/net_http` fork (`server_interceptor` branch)** over real HTTP.
- **In this remote environment**, pulling and building that fork (and wiring a
  real HTTP loopback) may not be feasible, and even if it builds, correctness of
  the *protocol* is best proven deterministically.

**Proposed approach (matches the scope you approved — "protocol design +
self-contained prototype"):**
- **Phase 1a — self-contained prototype (builds & runs here):** implement the
  Color core as a transport-agnostic C++17 library with a *simulated* in-process
  network that drops/duplicates/reorders requests and responses on demand. The
  fuzzy driver + correctness checker run against this to actually *prove* the
  safety/liveness properties deterministically and reproducibly (seeded RNG).
  This is the artifact that validates the design.
- **Phase 1b — real-transport integration (structured, may be build-only
  here):** wire the same Color core behind (a) the libcurl client wrapper and
  (b) the net_http interceptor, so the identical protocol logic runs over real
  HTTP. Kept behind the same interface as the simulated transport so nothing in
  the core changes.

The Color core is written once against a small transport interface; the
simulated network and the real (libcurl + net_http) transports are just two
implementations of that interface. This lets us prove correctness here *and*
satisfy the real-dependency requirement without duplicating protocol logic.

**Decision needed:** OK to proceed with the transport-abstraction approach
(prove correctness on the simulated transport first, then wire real
libcurl/net_http), or do you want the real net_http/libcurl path attempted
up front?

---

## 6. Proposed milestones (each a review checkpoint)

1. **This plan** — agree on understanding, layout, and the dependency approach
   (§5). ← we are here
2. **`docs/protocol.md`** — Phase I wire format, headers, ordering rule, and the
   safety-property argument (resolve Q1–Q5). Review before coding.
3. **Prototype skeleton** — transport interface + Color core + simulated lossy
   network + fuzzy driver + history checker (C++17, CMake). Prove S/L properties
   on seeded runs.
4. **Real transport** — libcurl client wrapper + net_http interceptor behind the
   same interface.
5. **Demo** — slowed, event-printing run + `demo/readme.md`.
6. **`docs/failover.md` + Phase II** — checkpoint data structure, JSON
   persistence, restart/replay, re-run verification, failover demo.

Commit granularity: you'll direct this as we go; my default is one reviewable
slice per commit to `main`.

---

## 7. Points I most want your confirmation on

- **A.** Is my statement of the safety invariant (§1: single identical total
  order, "server knows exactly what the client sees") the right north star?
- **B.** The acknowledgement header must express a *non-contiguous* set of
  seen responses (Q1). Agreed, or do you intend responses the client sees to
  always be contiguous (which would simplify things)?
- **C.** The dependency approach in §5 (transport abstraction; prove on
  simulated network first, then real libcurl/net_http).
- **D.** Repository layout in §3 (repo-root vs a `color/` subdir).
