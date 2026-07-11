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

## 2. Server checkpoint (the persisted message history)

Periodically (every `T` ms or every `N` commits — the `requirements.md` "save the
history as a JSON file, periodically") the server writes a checkpoint of its
committed message history and the state needed to resume:

| Field | Meaning |
|---|---|
| `committed_upto` | highest `Color-Seq` committed in order at checkpoint time |
| `history_hash` | running history hash at `committed_upto` |
| `history[]` | the committed request/response events in order, with response payloads: `{t:"R",seq}` / `{t:"r",seq,payload,hash}` |
| `buffer_from` | lowest `seq` still unacknowledged (responses `≥` this are retained for retransmission) |

`history[]` carries the **response payloads** because they are the app-visible
content that must survive; request payloads for already-committed requests are not
needed (those requests will not be reprocessed). The checkpoint is bounded by the
active window — see §8.

Checkpoint writes are atomic (write-temp-then-rename) so a crash mid-write leaves
the previous good checkpoint intact.

JSON layout (prototype):

```json
{
  "version": 1,
  "committed_upto": 120,
  "history_hash": "0x9f3a1c...",
  "buffer_from": 118,
  "history": [
    { "t": "R", "seq": 119 },
    { "t": "r", "seq": 119, "payload": "{\"srv_ts\":...}", "hash": "0x7b19..." },
    { "t": "R", "seq": 120 },
    { "t": "r", "seq": 120, "payload": "{\"srv_ts\":...}", "hash": "0x4c02..." }
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
   ├─ load checkpoint → committed_upto = K, history_hash, history[], buffer
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

Both the checkpoint's `history[]` and the replay envelope need only cover the
**active window** — from the acknowledged low-water (`Color-Ack-Base`) upward.
Everything the client has acknowledged is settled on both sides and can be
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
  replays, and the run continues; the existing checker must still pass (histories
  agree by exact prefix, exactly-once holds across the boundary, buffers stay
  bounded). Because the client object is otherwise untouched, this exercises the
  §5 recovery step and confirms the core client algorithm is unchanged.
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

## 11. Open questions for review

- **Q1. Recovery status code / signalling.** Proposed: a `5xx` (e.g. `503`) plus a
  `Color-Recover: from=<seq>` header and a `Color-Replay: 1` request marker. OK, or
  do you want a dedicated status code / endpoint for the replay?
- **Q2. Replay scope.** Replay from the server-requested `from=<seq>` (server-
  driven, minimal) vs. the client always replaying from its retained low-water
  (client-driven, simpler)? Proposed: server-driven `from`, client clamps to what
  it retains.
- **Q3. Checkpoint cadence & payloads.** Time-based, count-based, or both; and
  persist full `history[]` payloads (needed so the *server-side* checkpoint alone
  can answer already-acked retries) vs. only the unacknowledged tail (smaller,
  relies more on replay). Proposed: both cadences (whichever first); persist the
  unacknowledged tail plus `history_hash`, recover the rest via replay.
