# Color — Project Plan and Shared Understanding

Status: **REVIEWED — all decisions incorporated; awaiting final sign-off.**
This document summarizes my understanding of the problem (`spec.md`) and the
implementation requirements (`requirements.md`), updated with the review
answers. The former "open design questions" (Q1–Q5) are resolved as **D1–D6**
below and become the inputs to `docs/protocol.md`; the demo mechanism (§8) is
also decided. No open items remain — see §9 for the consolidated outcome.

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

### Resolved design decisions (from review round 1)

These were the open questions Q1–Q5; the review settled them as follows. They
are the inputs `docs/protocol.md` must formalize and prove.

- **D1. Acknowledgement encoding (was Q1).** The client sends an **acknowledgement
  array**: a **`base` id** meaning *"all responses with id < base have been
  received"* (a contiguous high-water mark), **followed by an explicit list of
  the individual larger, non-contiguous ids received beyond `base`**. Example:
  `base=124, [126, 130]` means the client has all responses < 124, plus 126 and
  130, but not 124, 125, 127–129. Chosen over bitmap/run-length because it is
  **human-readable** — important for a REST client that a person can drive by
  hand. This confirms §7-B: the ack must express a *non-contiguous* set.
- **D2. Id spaces (was Q2).** A **single monotonic id space assigned by the
  client** for requests. The **response simply echoes back the request id** it
  answers (no separate response-id space), unless a safety property later forces
  extra info. "Gaps in responses" therefore means: at a given instant the client
  holds responses for a non-contiguous subset of the request ids it has sent
  (because parallel requests complete out of order).
- **D3. History encoding (refines D2).** In the ordered message history, a
  request appears as its bare id and a response appears as the request id with a
  **`rsp-` prefix** (string or numeric tag). Example history as generated/
  delivered:
  `{ … 123, rsp-123, 234, 256, rsp-256, rsp-234 … }` — i.e. 234 and 256 were
  issued (parallel), 256 answered before 234.
- **D4. Buffer-release rule (was Q3).** When the server receives a request `X`
  whose acknowledgement (dependencies) includes an earlier response `Y`, the
  server may safely **drop buffered response `Y`** — the client has provably
  already received `Y` (the response for request `Y`). This bounds the server
  retransmission buffer (L2), given continued client progress.
- **D5. History hashing for verification (was Q4).** Both sides maintain the
  request/response history as an ordered array of ids (in total order) and a map
  **`{ req/rsp-id → running hash of the history up to and including that id }`**.
  Each request/response **piggybacks the sender's current history hash**; the
  receiver looks up the same id in its own map and compares hashes, giving
  efficient incremental verification that both histories agree on every prefix.
- **D6. Total-order definition + proof obligation (was Q5).** Requests and
  responses live in **one interleaved history**, ordered as they are *generated
  or delivered*:
  - **Client side:** requests are ordered as they are generated; responses are
    ordered as they are received.
  - **Server side:** each response's position is **derived from the request that
    acknowledges it** (the server can't observe the client's receive instant
    directly).
  `protocol.md` must **prove both sides derive the identical sequence.** Note the
  subtlety to nail in that proof: a single acknowledging request can newly-ack a
  *set* of responses, and the server sees only the set, not the client's
  intra-set receive order — so the canonical rule for ordering responses
  newly-acked by the same request (e.g. by ascending id) must be defined so that
  it coincides with the client's realized order. The D5 hash is what *detects*
  any divergence at runtime; the proof is what *guarantees* there is none.

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
  hash-carried-on-each-message scheme (**D5**).
- Run up to ~5 min. **Client and server run locally on the same VM** (confirmed
  in review — no cross-machine setup); optionally inject server processing delay.

### Demo
- Reuse the fuzzer as the demo, but slow the message rate, run continuously, and
  print client/server events to make the protocol legible.
- `readme.md` with full install/build/run instructions; **cmake only**.
- Transport/mechanism decided — see §8.

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

## 5. Dependency & environment approach — DECIDED

Review outcome: **"go with your proposal."** We use the transport-abstraction
approach, and client + server run **locally on the same VM**.

- **As specified**, the client depends on **libcurl** and the server on the
  **`wenbozhu2011/net_http` fork (`server_interceptor` branch)** over real HTTP.
- **Chosen approach:** the Color core is written **once** against a small
  transport interface; the simulated network and the real (libcurl + net_http)
  transports are two implementations of that interface. Prove correctness on the
  simulated transport first, then wire the real transports — without duplicating
  protocol logic.

- **Phase 1a — self-contained prototype (builds & runs here):** implement the
  Color core as a transport-agnostic C++17 library with a *simulated* in-process
  network that drops/duplicates/reorders requests and responses on demand. The
  fuzzy driver + correctness checker run against this to *prove* the
  safety/liveness properties deterministically and reproducibly (seeded RNG).
  This is the artifact that validates the design.
- **Phase 1b — real-transport integration:** wire the same Color core behind
  (a) the libcurl client wrapper and (b) the net_http interceptor, running over
  an HTTP loopback on the same VM. Kept behind the same interface as the
  simulated transport so nothing in the core changes.

---

## 6. Milestones and current status

1. ✅ **This plan** — understanding, layout, dependency approach (§5), demo (§8)
   all reviewed and confirmed. *(commits 5ee870f → 7fc3f3b)*
2. ✅ **`docs/protocol.md`** — Phase I wire format, headers, ordering rule, and
   the D1–D6 safety-property argument; revised per review (headers-only,
   version dropped, spec-gap §0, `Color-Hash` optional). *(f221ff0 → 4983b08)*
3. ✅ **Prototype skeleton (this milestone)** — transport-agnostic Color core
   (`src/core/`, compiled library), simulated lossy network, fuzzy driver, and
   invariant checker (`verification/`), C++17 + CMake. Proves the safety **and**
   liveness properties on seeded, reproducible runs; the checker is itself
   validated by a negative test. Requests arrive as a **Poisson process**
   (stochastic arrivals, capped by the flow-control window) so the interleavings
   are genuinely varied. See `verification/plan.md`; build/run in
   `verification/readme.md`.
   - Status: **100/100 seeds pass** at default settings (~98k requests through
     ~154k drops + ~36k duplicates); passes under `--drop 0.8`; buffers stay
     bounded. `ctest` wires a smoke + high-loss suite. ← **done**
4. ✅ **Real transport (this milestone)** — the same core behind real HTTP:
   - `src/client/` — libcurl client with a failure-injection wrapper (drops the
     request or the response); transport-level retry re-POSTs the identical
     request. **Builds and runs in this environment** (verified end-to-end
     against a loopback echo server: retransmits through heavy injected drops
     and completes every message with correct `Color-Seq` round-trip).
   - `src/server/` — Color as a **net_http request interceptor** (reusable
     framework library) driving the server core; parks out-of-order requests and
     completes replies in committed order. **Builds via CMake only** —
     `cmake/BuildNetHttp.cmake` compiles the net_http fork (`server_interceptor`
     branch) and abseil are pulled in by FetchContent; libevent + zlib are
     system deps. **Build- and run-verified in this environment.**
   - **End-to-end verified:** fresh `color_server` + `color_client`, 40 messages
     across 4 parallel senders with 40% drop on both directions and `--hash` on
     — completes with **0 history-hash mismatches** on both client and server
     (exercises out-of-order holding, in-order commit, exactly-once, and history
     agreement over real HTTP).
   - CMake builds `color_client` when libcurl is found and `color_server` when
     libevent is found (`-DCOLOR_BUILD_SERVER=OFF` to skip); the core/
     verification build is unaffected. See `src/readme.md`. ← **done**
5. ⬜ **Demo** — slowed, event-printing run over the real transport +
   `demo/readme.md` (mechanism per §8). *(the `--verbose` event trace in the
   harness is the basis for this.)*
6. ⬜ **`docs/failover.md` + Phase II** — checkpoint data structure, JSON
   persistence, restart/replay, re-run verification, failover demo.

Commit granularity: you direct this as we go; my default is one reviewable slice
per commit to `main`.

---

## 7. Confirmation points — RESOLVED

- **A. Safety invariant** (single identical total order; "server knows exactly
  what the client sees") → **confirmed** as the north star.
- **B. Non-contiguous acknowledgement** → **confirmed** ("the former"): the ack
  must express a non-contiguous set of seen responses (see **D1**).
- **C. Dependency approach** (§5 transport abstraction; simulated first, then
  real libcurl/net_http) → **confirmed** ("go with your proposal").
- **D. Repository layout** (§3) → no objection raised; proceeding with the
  **repo-root layout** (`docs/`, `client/`, `server/`, `verification/`,
  `demo/`). Say the word if you'd rather nest everything under a `color/` subdir.

---

## 8. Demo mechanism — DECIDED

Review outcome: **real-transport demo, on the same VM, manually runnable, with
client-side-only failure injection via the libcurl wrapper.**

- **Split by purpose.** The **verification harness** runs the Color core over
  the **simulated transport** (deterministic, seeded, fast) — that's where we
  *prove* correctness. The **demo** runs the **same Color core over the real
  transport** (libcurl client ↔ net_http server, HTTP loopback on one VM). Both
  share the identical Color core; only the transport implementation differs.
- **Manually runnable.** Start the net_http server process and the libcurl
  client process by hand (documented in `demo/readme.md`, cmake only). No
  orchestration harness — the operator drives it.
- **Failures are client-side only.** All fault injection happens in the
  **libcurl wrapper** (randomly drop the outgoing request or the incoming
  response), which triggers an immediate transport-level HTTP retry. The
  net_http server injects no failures; per `requirements.md` this is sufficient
  to exercise every protocol path.
- **Demo behaviour.** Slow the request rate (e.g. ~1/sec), run non-stop, and
  print a readable per-message event log on **both** client and server showing:
  request id, the acknowledgement array (`base` + extra ids, per **D1**),
  whether the request/response was injected-dropped and retried, the resulting
  ordered history, and the piggybacked history hash (**D5**) with a ✓/✗ match
  marker.
- **Phase II failover demo.** Manually kill the net_http server, restart it on
  the same port; it reloads the checkpointed JSON history; the libcurl client —
  still retrying, unaware — resumes; the event log shows the "chat" continuing
  across the restart, with history hashes still matching. (A real process/socket
  is exactly why the demo uses the real transport.)

Implication: the **demo** depends on the real libcurl + net_http build
(Phase 1b); **verification** does not (Phase 1a, self-contained).

---

## 9. Review outcome — summary

- Q1–Q5 resolved as **D1–D6** (§2); ack encoding, id reuse, history encoding,
  buffer-release, hashing, and the total-order proof obligation are all pinned.
- §7 A/B/C confirmed; D defaulted to repo-root layout.
- §5 dependency approach and single-VM co-location confirmed.
- §8 demo decided: real-transport, same VM, manually runnable, client-side-only
  failure injection via the libcurl wrapper.
- **No open items remain.** Pending your final review of this plan, the next
  step is to write **`docs/protocol.md`** formalizing D1–D6, including the
  §2/D6 total-order proof.
