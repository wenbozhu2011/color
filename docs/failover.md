# Color — Phase II: Server Failover (design)

Status: **DRAFT for review.** This document designs server failover on top of the
Phase I protocol (`docs/protocol.md`). It is a design/protocol document; the
checkpoint implementation and the failover verification/demo follow in the
Phase II milestone.

---

## 0. Headline: failover requires **no client-side changes**

**The client is completely unaware of failover. Nothing about the client —
its wire format, its state, or its behaviour — changes for Phase II.** This is
the central property of the design, so it comes first.

Why it holds:

- From the client's vantage point, a server failover is **indistinguishable from
  ordinary message loss plus latency**: for a while some requests/responses do
  not get through (the old server is gone, the new one isn't up yet, or is
  rebuilding), and then they do. The Phase I client already copes with exactly
  this by **retransmitting the identical request until a response arrives** and
  **ignoring duplicate responses**. No new signal, status code, or message type
  is introduced on the client.
- The client already puts everything a recovering server needs **on every
  request**: the `Color-Seq` id and the `Color-Ack-Base`/`Color-Ack-New`
  acknowledgement. The new server reads these exactly as the old one did.
- Recovery is achieved **entirely on the server side** by one rule (§2) plus a
  checkpoint (§3). Because that rule guarantees the new server already holds
  everything the client could still be waiting on, the client never has to
  "help" the server catch up.

In particular this design **does not** use the `5xx`-and-replay mechanism
sketched in `spec.md` / `protocol.md` §12, because that *would* be a client-side
change (the client would have to recognise the status and re-send a special
history payload). §7 explains what we do instead and why it is preferable here.

> The rest of this document justifies the headline. If you read only one more
> section, read §2 (the rule) and §5 (why the three possible fates of a request
> are all covered without the client doing anything new).

---

## 1. Goal and failure model

- **One logical conversation, one server *role*.** A single client talks to a
  single logical server. At any moment exactly one server process serves the
  conversation; a failover replaces that process with another (crash + restart on
  the same port, a drain/migration, or a new process on a new host that requests
  are routed to).
- **Crash-stop.** The serving process may stop at any instant. The requirement is
  that a replacement process, started from the persisted checkpoint, resumes the
  conversation with all Phase I safety and liveness properties intact.
- **Checkpoint durability is assumed.** The checkpoint (§3) is written to storage
  the replacement can read (a local file for the prototype/demo; shared/durable
  storage in production). Total loss of the checkpoint is data loss beyond the
  scope of failover — see §10.

Safety/liveness targets are unchanged from Phase I: the client and server agree
on one identical total order, requests are processed exactly once, and the
conversation keeps making progress.

---

## 2. The one rule: durable-before-release (the checkpoint fence)

The server may **release (send) a response to the client only after the commit
that produced it has been persisted in a checkpoint.**

Concretely, the Phase I server loop (`protocol.md` §6) gains a fence:

1. Commit requests in `Color-Seq` order and generate their responses into a
   *pending-release* set (as today), but **do not send them yet**.
2. **Periodically** (every `T` ms or every `N` commits — this is the
   `requirements.md` "save the history as a JSON file, periodically") write the
   checkpoint (§3), which includes `committed_upto` and every unacknowledged
   response.
3. **After** the checkpoint write succeeds, release the responses it just made
   durable.

The single consequence that powers everything else:

> **If the client has received a response for `seq`, then `seq`'s commit is
> durable in the checkpoint.** (A response is only released after it is
> persisted.)

Equivalently: the client can never be "ahead" of the checkpoint. Anything the
client already knows, the next server already knows too.

Checkpointing stays **periodic/batched** (not a disk write per request); the
fence only delays a response's *release* to the next checkpoint boundary, adding
at most one checkpoint interval of latency. That interval is the tunable
throughput/latency/coverage knob (§8, §10).

---

## 3. Checkpoint data structure

The checkpoint captures exactly the **active window** of the conversation — the
part that is not yet settled — plus the running summary needed to continue.
Everything the client has already acknowledged is settled and need not be
replayed, so the checkpoint size is bounded by the in-flight window, not by the
conversation length (§8).

Logical contents:

| Field | Meaning | Used for |
|---|---|---|
| `committed_upto` | highest `Color-Seq` committed in order | resume ordering; classify retries |
| `history_hash` | running history hash at `committed_upto` | continue the hash chain (verification) |
| `buffer[]` | committed-but-**unacknowledged** responses: `{seq, payload, hash}` | answer retries exactly-once; echo `Color-Hash` |
| `low_water` *(optional)* | all responses `< low_water` are acked+released | compaction bookkeeping |
| `history[]` *(optional)* | full ordered events `{t:"R"/"r", seq}` | audit / verification deep-compare |

Reconstruction rule on load: everything `≤ committed_upto` is committed; those
still in `buffer[]` are committed-but-unacked; those `≤ committed_upto` **not** in
`buffer[]` are committed-and-acked (their responses were released and dropped).
That three-way classification is all the new server needs to serve any retry
(see §5). `history[]` is not required for correctness — the active window
(`committed_upto`, `history_hash`, `buffer[]`) suffices; it is persisted in the
prototype so the verification harness can deep-compare full histories.

JSON layout (prototype):

```json
{
  "version": 1,
  "committed_upto": 128,
  "history_hash": "0x9f3a1c...",
  "low_water": 121,
  "buffer": [
    { "seq": 121, "payload": "{\"srv_ts\":...}", "hash": "0x7b19..." },
    { "seq": 125, "payload": "{\"srv_ts\":...}", "hash": "0x4c02..." }
  ],
  "history": [ {"t":"R","seq":1}, {"t":"r","seq":1}, "…" ]
}
```

Note `Color-Ack-Base` is already the client's compact statement of "all responses
below this are received", so the server's `low_water`/`buffer` boundary is driven
by the same acknowledgements it already processes in Phase I — no new bookkeeping.

---

## 4. Failover sequence

```
… old server: commit K+1..M, checkpoint(committed_upto=M', buffer=…), release durable responses, CRASH …
        │
        │  (client keeps sending / retransmitting; connections fail → retried)
        ▼
new server starts on the same port
   ├─ load checkpoint  → committed_upto, history_hash, buffer[]
   ├─ StartAcceptingRequests
   ▼
client's in-flight retries + new requests arrive (unchanged Phase I requests)
   ├─ apply Color-Ack-Base/New → release now-acked buffered responses (Phase I D-rule)
   ├─ seq ≤ committed_upto and in buffer      → resend the buffered response (no reprocess)
   ├─ seq ≤ committed_upto and not in buffer  → already acked → no-op ack reply
   └─ seq > committed_upto                     → stage and commit in order, run app, buffer, (fence) release
```

The client does nothing special anywhere in this diagram; it is retransmitting
and acknowledging exactly as in Phase I.

---

## 5. Why zero client changes suffices — the three fates of a request

At the instant of the crash, consider any request `seq`. It is in exactly one of
three states, and each is handled by the new server using only Phase I client
behaviour (retry + ignore-duplicates):

1. **Never durably committed** (the old server never received it, or crashed
   before a checkpoint covered it). By the fence (§2) its response was never
   released, so **the client never received a response** → the client is still
   **retransmitting** `seq`. The new server has no record of it and processes the
   retry **fresh, exactly once**. ✓
2. **Durably committed, response released, not yet acknowledged.** The response is
   in the checkpoint `buffer[]`. If the client hadn't received it, it retries →
   the new server **resends the buffered response** (no reprocessing). If the
   client had received it, a stray retry likewise returns the buffered response,
   and the client **ignores the duplicate**. ✓
3. **Durably committed and acknowledged** (client has the response and acked it;
   the old server dropped it from the buffer). Then `committed_upto ≥ seq` in the
   checkpoint and the client **will not retry** `seq`. Nothing to do. ✓

**Liveness / no permanent gap.** Every acknowledged request is `≤ committed_upto`
(case 3, durable by the fence), and every request the new server still needs to
fill the range `> committed_upto` is unacknowledged, hence being retransmitted
(cases 1–2). So the new server eventually receives every request required to
advance `committed_upto` in order — there is no gap that only an un-retried
request could fill. This is the property the naïve "periodic checkpoint without
the fence" design lacks (a post-checkpoint *acked* request would be a gap the
client never resends); the fence is exactly what removes it **without** asking
the client to replay.

---

## 6. Interaction with the Phase I mechanisms

- **Exactly-once (`processed`/dedup).** Reconstructed from `committed_upto` +
  `buffer[]`: retries `≤ committed_upto` are served without invoking the
  application; the app resumes only for `seq > committed_upto`.
- **Buffer release (D4).** Unchanged. The client's `Color-Ack-Base`/`Color-Ack-New`
  on post-failover requests release the reloaded `buffer[]` entries just as in
  Phase I.
- **Total order & hash (§4/§7 of protocol.md).** The new server continues the
  committed history from `committed_upto` with `history_hash` as the running
  value, so the piggybacked `Color-Hash` continues to match the client's — hash
  verification survives the failover seamlessly, because durable-before-release
  guarantees the histories never diverged in the first place.
- **In-order commit / staging.** `pending_reqs` (out-of-order arrivals not yet
  committed) is **not** persisted; those requests were unacknowledged, so the
  client retransmits them and they re-stage naturally.

---

## 7. What we deliberately do NOT use: the `5xx` replay

`spec.md` sketches an alternative: on failover the server answers a request with
a special `5xx`, and the client re-sends its known request/response history in a
dedicated JSON-enveloped request so the new server can rebuild.

We **do not** adopt it, because recognising the `5xx` and constructing a replay
payload is a **client-side change** — precisely what §0 rules out. The
durable-before-release fence achieves the same recovery entirely server-side.

Trade-off (stated honestly):

- **This design (fence):** zero client changes; costs at most one checkpoint
  interval of added response latency, and requires the checkpoint to be written
  on the release path.
- **`5xx` replay:** no per-commit durability fence (cheaper server writes), but
  requires client cooperation and a new request type, and the replay payload can
  be large. It also only helps when the client still holds the relevant history.

Given the project's stated priority ("no client-side changes"), the fence wins.
The `5xx` path remains a possible future option for **cold recovery** (total
checkpoint loss, §10), where no server-side state survives and client replay is
the only source of truth — but that is out of scope for Phase II.

*(This supersedes the Phase II sketch in `protocol.md` §12, which will be updated
to point here.)*

---

## 8. Bounded checkpoint size (compaction)

The checkpoint stores only the **active window**: `committed_upto`,
`history_hash`, and the unacknowledged `buffer[]`. As the client acknowledges
responses (advancing `Color-Ack-Base`), settled entries drop out of `buffer[]`
and `low_water` advances. Therefore the checkpoint size is `O(in-flight window)`,
independent of total conversation length — the same boundedness argument as the
Phase I retransmission buffer (`protocol.md` §10, L2). The optional `history[]`
audit array is the only unbounded field and is a prototype/verification
convenience, not a protocol requirement; production compacts it to the running
`history_hash`.

The checkpoint **interval** trades three things: longer interval → less
persistence overhead but more added release latency and more post-crash
re-processing of never-released commits (case 1); shorter interval → the reverse.

---

## 9. Verification across failover, and the demo

- **Verification (simulated).** Extend the harness (`verification/`) to, mid-run,
  serialize the `ColorServer` to a checkpoint and reconstruct a **fresh**
  `ColorServer` from it (optionally dropping the un-fenced tail to model a crash
  between checkpoints), then continue driving the same client against the new
  server. The existing checker must still pass: histories agree (exact-prefix),
  exactly-once holds across the boundary, and buffers stay bounded. Because the
  client object is untouched across the swap, this also *demonstrates* the
  no-client-changes property in code.
- **Demo (real transport).** As in `demo/readme.md`: run `color_server` with
  periodic checkpointing to a JSON file; start `color_client`; **kill the server
  and restart it on the same port**; the new process loads the checkpoint and the
  "chat" continues, with `hash mismatches=0` straight through the restart. The
  client command line is identical to the Phase I demo — nothing about running
  the client changes.

---

## 10. Assumptions and limits

- **Checkpoint availability.** The replacement must be able to read the
  checkpoint (same disk for restart-in-place; shared/durable storage for
  migration to another host). This is assumed available and consistent.
- **Single-writer checkpoints.** Exactly one server writes the checkpoint at a
  time; failover is not concurrent (no two servers serving the same conversation
  simultaneously). A checkpoint write should be atomic (write-temp-then-rename)
  so a crash mid-write leaves the previous good checkpoint intact.
- **Cold recovery (total checkpoint loss) is out of scope.** With no surviving
  state, only client replay (the `5xx` path, §7) could rebuild the conversation —
  and that would reintroduce a client-side change. Phase II assumes the
  checkpoint survives.
- **Application idempotence for case 1.** A request that was received but never
  durably committed is reprocessed after failover. This is exactly-once *from the
  client's view* (it never saw a prior response), but the application's own side
  effects for that `seq` run on the new server. The reference echo app is pure;
  stateful apps should keep their effects inside the committed-history path so
  durability covers them too.

---

## 11. Open questions for review

- **Q1. Fence vs. lighter variant.** Recommended: durable-before-release (exact
  history preserved, hash verification survives). A lighter alternative keeps
  purely-periodic checkpoints (no release fence) and, on failover, advances over
  any post-checkpoint *acked* gap using the client's `Color-Ack-Base`
  ("phantom-fill") — also zero client changes, cheaper, but the new server no
  longer holds the exact tokens for the gap, so history/hash must be re-baselined
  across the failover. Do you want the exact-history fence, or the cheaper
  re-baseline variant?
- **Q2. Checkpoint scope.** Persist the full `history[]` (simplest; lets
  verification deep-compare) or only the active window + `history_hash`
  (bounded, production-shaped)? Proposed: full history in the prototype behind a
  flag, active-window by default.
- **Q3. Checkpoint cadence.** Time-based (`T` ms), count-based (`N` commits), or
  both? Proposed: both, whichever comes first, tunable via flags.
