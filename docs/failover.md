# Color — Phase II: Server Failover (design)

Status: **DRAFT for review.** This document designs server failover on top of the
Phase I protocol (`docs/protocol.md`). It is a design/protocol document; the
checkpoint implementation and the failover verification/demo follow in the
Phase II milestone.

---

## 0. Headline: the client's **core protocol/algorithm is unchanged**

Failover adds a recovery interaction, and that interaction does involve the
client — but **the client's core conversational algorithm is unchanged.** The
client still assigns monotonic `Color-Seq` ids, carries the same
`Color-Ack-Base`/`Color-Ack-New` acknowledgement, retransmits until a response
arrives, and ignores duplicates — byte-for-byte as in Phase I. Steady-state
traffic is identical.

What failover adds is a single **recovery step layered on top**: when a
replacement server signals (with a `5xx`) that it is missing history the client
already holds, the client **resends its known request/response history** so the
new server can recover the responses it lost. Together, the `5xx` signal and the
client's history replay give a **seamless** recovery — the conversation
continues without the application noticing.

Why the client must participate (this is the point I want to get right):

- A periodic checkpoint can lag the crash. After failover the new server is
  missing the tail of the history — in particular **responses the client already
  received but that were generated after the last checkpoint.**
- Requests in that tail that the client has *not* yet had answered are recovered
  for free: the client is still **retransmitting** them, so the new server simply
  reprocesses them.
- But responses the client *has* received are a different matter: the client will
  **not** retransmit an already-answered request, so the new server can never
  regenerate those responses on its own — and even if it reprocessed the request,
  it might produce a *different* response, breaking the agreed history. **The
  client is the only surviving holder of those exact responses.** Recovery
  therefore requires the client to resend them.

So: not "no client changes", but "**the core algorithm is the same; failover adds
one recovery step (respond to `5xx` by replaying known history).**"

---

## 1. Goal and failure model

- **One logical conversation, one server *role*.** A single client talks to a
  single logical server. At any moment one server process serves the
  conversation; a failover replaces it with another (crash + restart on the same
  port, a drain/migration, or requests routed to a new process).
- **Crash-stop.** The serving process may stop at any instant. A replacement,
  started from the persisted checkpoint plus the client's replay, must resume the
  conversation with all Phase I safety and liveness properties intact.
- **Checkpoint is periodic and may lag.** The server saves the message history to
  storage periodically (a local JSON file for the prototype/demo; durable/shared
  storage in production). The window between the last checkpoint and the crash is
  recovered at runtime from the client (§3–§4).

Safety/liveness targets are unchanged from Phase I: client and server agree on one
identical total order, requests are processed exactly once, and the conversation
keeps making progress.

---

## 2. Server checkpoint (the persisted active state)

Periodically — every `T` ms **or** every `N` commits, whichever comes first (the
`requirements.md` "save the history as a JSON file, periodically") — the server
writes a checkpoint. Per the review decision (§11-Q3), the checkpoint stores only
the **active (unacknowledged) tail plus a running summary**, not the full settled
history:

| Field | Meaning |
|---|---|
| `committed_upto` | highest `Color-Seq` committed in order at checkpoint time |
| `history_hash` | running history hash at the committed frontier — lets the rebuilt history re-converge and continue the `Color-Hash` chain |
| `buffer[]` | the committed-but-**unacknowledged** responses `{seq, payload, hash}`, retained so the new server can resend them to a client still waiting |

The unacknowledged responses **must** be persisted here: the client never received
them, so it cannot replay them — the server is their only holder (mirror image of
the *answered* responses, which only the client holds; §3). Everything the client
has already acknowledged is settled and is compacted away — represented only by
`committed_upto` + `history_hash`; if the new server turns out to need part of that
settled or post-checkpoint region, the client replays it (§4). The checkpoint is
therefore bounded by the active window (§8).

Checkpoint writes are atomic (write-temp-then-rename) so a crash mid-write leaves
the previous good checkpoint intact.

JSON layout (prototype):

```json
{
  "version": 1,
  "committed_upto": 120,
  "history_hash": "0x9f3a1c...",
  "buffer": [
    { "seq": 118, "payload": "{\"srv_ts\":...}", "hash": "0x7b19..." },
    { "seq": 120, "payload": "{\"srv_ts\":...}", "hash": "0x4c02..." }
  ]
}
```

---

## 3. What is missing after a failover, and who recovers it

Let the checkpoint stop at `committed_upto = K`, while the old server had actually
progressed to `M ≥ K` before crashing. The new server loads `K` and is missing
`(K, M]`. Split that tail by whether the client already has the response:

- **Unanswered requests** (client has not received a response). The client is
  **still retransmitting** them. The new server reprocesses each retry and
  produces its response — correct, because the client never saw a prior response
  for that `seq` (exactly-once from the client's view). *Recovered by ordinary
  retransmission; no new mechanism.*
- **Answered responses** (client received the response, generated after the last
  checkpoint). The client will **not** retransmit the request, so the new server
  cannot regenerate the response, and must not invent a different one. *Recovered
  only by the client resending it* — this is the `5xx`-and-replay path (§4).

The client's acknowledgement is what exposes the second case: a post-failover
request carries `Color-Ack-Base = B` (and `Color-Ack-New`) asserting the client
holds responses the new server has no record of (`B-1 > K`). That mismatch is the
trigger.

---

## 4. The `5xx`-and-replay recovery protocol

1. **Detect.** The new server receives a request whose acknowledgement references
   history it lacks — `Color-Ack-Base` (or an id in `Color-Ack-New`) names a
   response `seq` it has not committed. It cannot honour the request yet.
2. **Signal.** The server replies with a **`5xx`** (a dedicated recovery status,
   e.g. `503` carrying `Color-Recover: from=<K+1>`), telling the client the lowest
   `seq` from which it needs history. This is the only new status the client must
   recognise.
3. **Replay.** The client resends its **known request/response history from
   `K+1`** as a single JSON-enveloped **replay request** (`Color-Replay: 1`): the
   ordered events it knows, with the **response payloads and hashes** it received.
   This is exactly the "encode multiple requests and responses as message history
   in a single request body" from `requirements.md`.
4. **Rebuild.** The server ingests the replay: it appends the replayed events to
   its committed history in order, adopting the client's exact response payloads,
   advancing `committed_upto` and `history_hash`, and re-populating the
   retransmission buffer for any responses still unacknowledged. It replies `200`
   to the replay.
5. **Resume.** The client continues normally — retransmitting still-pending
   requests and issuing new ones. The new server now has the history it needs to
   order and answer them. Steady state is Phase I again.

Replay envelope (JSON body of the replay request):

```json
{
  "from": 121,
  "events": [
    { "t": "R", "seq": 121 },
    { "t": "r", "seq": 121, "payload": "…", "hash": "0x…" },
    { "t": "R", "seq": 122 },
    { "t": "R", "seq": 123 },
    { "t": "r", "seq": 123, "payload": "…", "hash": "0x…" },
    { "t": "r", "seq": 122, "payload": "…", "hash": "0x…" }
  ]
}
```

The replay is **idempotent**: if the server already advanced past part of it (or a
replay is duplicated by retransmission), the server ignores events at or below its
current `committed_upto`. A lost replay is simply re-triggered by the next `5xx`.

---

## 5. Scope of the client-side change

The change to the client is deliberately small and **orthogonal to the core
algorithm**:

- **Unchanged:** `Color-Seq` assignment, `Color-Ack-Base`/`Color-Ack-New`
  construction, the total-order/commit logic, retransmission, duplicate handling —
  every steady-state request is byte-identical to Phase I.
- **Added:** (a) recognise the `5xx`/`Color-Recover` response; (b) build a replay
  envelope from the request/response history the client already retains; (c) POST
  it; then continue. The client already *keeps* its received responses (it commits
  them into its own history), so replay is a read-out of existing state, not new
  bookkeeping.

A "manual" REST client that does not implement recovery still works against a
server that never fails over (Phase I), and degrades to a stall (not a safety
violation) if it meets a `5xx` it ignores — so the recovery behaviour is an
opt-in capability, not a change to the wire contract for the steady state.

---

## 6. Failover sequence

```
old server: commit … K (checkpoint) … K+1..M, release responses, CRASH
        │        (client keeps sending / retransmitting; connections fail → retried)
        ▼
new server starts on the same port
   ├─ load checkpoint → committed_upto = K, history_hash, buffer[] (unacked tail)
   ├─ StartAcceptingRequests
   ▼
client request P arrives (ack_base = B)
   ├─ B-1 ≤ K → server already has the referenced history → serve normally
   └─ B-1 > K → server is missing (K, B-1]  → reply 5xx Color-Recover: from=K+1
            │
            ▼
   client → POST replay envelope (events K+1 … with response payloads)
            │
            ▼
   server rebuilds committed history to the client's frontier, replies 200
   client resumes: retransmits pending requests, issues new ones (Phase I steady state)
```

---

## 7. Interaction with the Phase I mechanisms

- **Exactly-once.** After rebuild, `committed_upto` + the buffer classify retries
  exactly as in Phase I: `≤ committed_upto` and buffered → resend; `≤` and not
  buffered → already acked (no-op); `>` → new. Unanswered requests in `(K,M]` that
  the client retransmits are (re)processed once by the new server, since the client
  never held a response for them.
- **Total order & hash.** The replay carries the client's exact ordering and
  response payloads, so the rebuilt history is identical to the client's and the
  running `history_hash` re-converges; the piggybacked `Color-Hash` matches again
  after rebuild. Recovery restores the *exact* history, not an approximation.
- **Buffer release (D4).** Unchanged; post-failover acknowledgements release the
  rebuilt buffer as usual.
- **Staging.** Out-of-order, not-yet-committed requests are not persisted; they are
  unacknowledged, so the client retransmits them and they re-stage naturally.

---

## 8. Bounding checkpoint and replay size

The checkpoint (its unacknowledged `buffer[]`) and the replay envelope both cover
only the **active window** — from the acknowledged low-water (`Color-Ack-Base`)
upward. Everything the client has acknowledged is settled on both sides and is
compacted to `committed_upto` + `history_hash`; the client need not retain, and
need not replay, below that frontier. So both are `O(in-flight window + checkpoint
lag)`, independent of total conversation length — the same boundedness argument as
the Phase I retransmission buffer (`protocol.md` §10, L2).

The checkpoint **interval** is the tunable knob: a longer interval means cheaper
persistence but a larger post-crash gap to recover (more retransmission and a
larger replay); a shorter interval, the reverse.

---

## 9. Verification across failover, and the demo

- **Verification (simulated).** Extend the harness (`verification/`) to, mid-run,
  serialize the `ColorServer` to a checkpoint, discard the post-checkpoint tail to
  model a crash, and reconstruct a **fresh** `ColorServer` from the checkpoint —
  then keep driving the **same** client against it. The client hits the `5xx`,
  replays, and the run continues. Because the new server intentionally does not
  retain the settled prefix, correctness across the boundary is checked by
  **history-hash agreement** (the piggybacked `Color-Hash` re-converges after
  replay), exactly-once holding across the boundary, and bounded buffers; tokens
  committed after the rebuild are also exact-prefix compared. Because the client
  object is otherwise untouched, this exercises the §5 recovery step and confirms
  the core client algorithm is unchanged.
- **Demo (real transport).** As in `demo/readme.md`: run `color_server` with
  periodic checkpointing to a JSON file; start `color_client`; **kill the server
  and restart it on the same port**; the client's next request triggers the `5xx`,
  the client replays, and the "chat" continues with `hash mismatches=0` through the
  restart. The client is invoked identically to the Phase I demo.

---

## 10. Assumptions and limits

- **Checkpoint availability.** The replacement must read the checkpoint (same disk
  for restart-in-place; shared/durable storage for migration). Assumed available
  and consistent.
- **Single active server.** Exactly one server serves the conversation at a time;
  failover is not concurrent.
- **Client retention.** The client must retain the responses it may need to replay
  — i.e. its history above the acknowledged low-water (bounded, §8). If the
  checkpoint is older than the client's retained frontier, recovery cannot bridge
  the gap (a degenerate case; keep checkpoints within the retained window).
- **Application effects for reprocessed requests.** An unanswered request in the
  gap is reprocessed on the new server; this is exactly-once from the client's
  view (it saw no prior response), but the app's side effects for that `seq` run on
  the new server. The reference echo app is pure; stateful apps should keep effects
  inside the committed-history path so checkpoint + replay cover them.

---

## 11. Resolved decisions (review)

- **D1. Recovery signalling → `503`.** The server signals recovery with HTTP
  **`503`** carrying `Color-Recover: from=<seq>`; the client's replay is a POST
  carrying `Color-Replay: 1` and the JSON envelope of §4. No dedicated status code
  or endpoint — the marker headers keep it on the existing path.
- **D2. Replay scope → server-driven.** The server dictates the replay start via
  `Color-Recover: from=<seq>`; the client replays from that `seq`, clamped to what
  it still retains (§10). Minimal replay, server decides how far back it needs.
- **D3. Checkpoint cadence & scope → as proposed.** Checkpoint on **both** a time
  interval and a commit count, whichever comes first. Persist only the
  **unacknowledged tail** (`buffer[]`) plus `committed_upto` and `history_hash`
  (§2); everything else is recovered via replay.
