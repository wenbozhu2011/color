# Color — Phase I REST Protocol Design

Status: **DRAFT for review.** This document formalizes the Phase I ("basic
version", no failover) Color protocol, turning the decisions **D1–D6** recorded
in `docs/plan.md` into a concrete wire format, client/server behaviour, worked
HTTP examples, and a proof of the core safety invariant. Phase II failover is
out of scope here (see the forward pointer in §12 and, later, `docs/failover.md`).

Reviewer's eye, please, on the **§11.1 receive-order refinement** — it is the
one place the design goes beyond the literal D1 encoding, and it is the crux of
the D6 proof.

## Contents

0. Where this design corrects or extends `spec.md`
1. Terminology and identifiers
2. HTTP mapping and header summary
3. The three mechanisms (id, acknowledgement, history hash)
4. The committed history and its total order (D6)
5. Client behaviour
6. Server behaviour
7. History hashing (D5), precisely
8. Worked examples (HTTP wire dumps)
9. Safety argument (S1–S5 and the invariant)
10. Liveness and bounded buffers (L1–L2)
11. Design decisions and notes
12. Forward pointer to Phase II

---

## 0. Where this design corrects or extends `spec.md`

Two points where `spec.md` is either inaccurate or silent. Both are
load-bearing for correctness, so they are called out up front.

1. **Response acknowledgement must convey *order*, not just a set.** `spec.md`
   states that the ordering of received responses is significant (its property 3
   / S4), yet the "sequence of responses known to the client" it describes reads
   as a *set* of ids. A set cannot express the order in which the client
   received responses that completed **out of order** (e.g. `rsp-256` received
   before `rsp-234`). This design therefore has the acknowledgement carry an
   **ordered** delta of newly-received responses (`Color-Ack-New`, §11.1), not
   just a cumulative set. Without it, client and server would order
   jointly-acknowledged responses differently and the single-total-order
   invariant would break.

2. **Duplicated messages, not just lost ones.** `spec.md` §5 frames failures as
   *loss* only ("requests or responses may be lost … the client will retry"). It
   does not mention that retries and the network also cause **duplicate
   delivery** of requests (and responses). The protocol must be idempotent under
   duplication: the server dedups requests by `Color-Seq` and never reprocesses
   (S5), and the client ignores duplicate responses. The verification harness
   deliberately **duplicates** messages (per `requirements.md`), so this is a
   first-class case here, not an afterthought.

---

## 1. Terminology and identifiers

- **Conversation.** One logical client talking to one logical server endpoint
  over a series of non-streamed HTTP POST RPCs.
- **Request id / `seq`.** A strictly increasing integer the client assigns to
  each *distinct* request, starting at `1` and incrementing by exactly one
  (`1, 2, 3, …`). The id space is **contiguous** — there are no skipped ids.
  A *retry* of a request reuses the same `seq` and the same headers/body (it is
  byte-for-byte identical); it is not a new request. (**D2**)
- **Response.** The server's answer to request `seq`. A response **echoes back
  the request id** — there is no separate response-id space. (**D2**) In the
  message history a request appears as the token `R<seq>` and its response as
  `r<seq>` (the "`rsp-`" prefix of **D3**).
- **Received (by the client).** A response is *received* the instant the client
  gets the HTTP response for that `seq` for the first time.
- **Acknowledged (to the server).** A response `r<i>` is *acknowledged* once the
  client has told the server, via a later request's acknowledgement fields, that
  it received `r<i>`.
- **Committed.** A token (`R<seq>` or `r<i>`) is *committed* once it has been
  placed at its final position in the total-ordered history (§4). Both sides
  commit tokens; the server's committed history is always a prefix of the
  client's.

Because parallel requests may be in flight and their responses can complete out
of order, at any instant the set of *received* responses can be **non-contiguous**
in `seq` (e.g. received `{1, 2, 4}`, missing `3`). This is the "gaps in
responses" of the requirements. Request ids themselves never have gaps.

---

## 2. HTTP mapping and header summary

- **Method.** Every Color message is an HTTP `POST`; the body is the opaque
  application payload (JSON in the reference app). POST is safe to retransmit
  here precisely because Color provides exactly-once semantics.
- **Path is the application's choice.** Color neither defines nor constrains the
  request URL/path — that is entirely up to the application. Color is carried
  **through HTTP headers only**, so it layers onto any POST endpoint. The
  examples below use a placeholder path (`POST …`).
- **One request → one response.** No batching (Phase I). Parallel requests use
  separate concurrent POSTs.
- **Transport-level retry.** Request/response loss is handled *below* Color by
  the HTTP client (an immediate retry of the identical POST). Color logic never
  sees a "failure"; it only ever sees delivered messages, possibly duplicated or
  reordered. (Matches requirements: no "rich client".)

### Request headers

| Header | Meaning | Refs |
|---|---|---|
| `Color-Seq` | This request's id `n` (contiguous, monotonic). | D2 |
| `Color-Ack-Base` | Cumulative low-water `b`: **all responses with id `< b` have been received.** `1` initially. | D1, D4 |
| `Color-Ack-New` | The **ordered** list of response ids received since the previously-generated request, **in receipt order** (may be empty). This is what conveys the client's receive order to the server. | D1, D6 |
| `Color-Hash` | The client's running history hash **after committing this request's own token `R<n>`** (i.e. after appending the `Color-Ack-New` responses and then `R<n>`). | D5 |

### Response headers

| Header | Meaning | Refs |
|---|---|---|
| `Color-Seq` | Echo of the request id `n` this response answers. | D2 |
| `Color-Hash` | The server's running history hash **after committing `R<n>`** (the prefix ending at the request being answered; the response's own `r<n>` token is not yet committed — see §4/§7). | D5 |

**Versioning** is intentionally omitted from Phase I for simplicity. A deployment
that needs to evolve the wire format can add a version header (e.g.
`Color-Version`) without affecting any of the rules here.

Application-level results (including application errors) travel in the response
**body**; HTTP `200` is used for any *delivered* Color response. `5xx` is
reserved for Phase II failover signalling and is unused in Phase I.

All header integers are decimal ASCII; `Color-Ack-New` is a comma-separated list
with no spaces (empty header = empty list). Everything is human-readable so a
person can drive a Color exchange by hand with `curl` (a requirement from the
review of D1).

---

## 3. The three mechanisms

Color rides on three pieces of metadata. Each maps to a decision from the plan.

1. **Request id (`Color-Seq`).** Idempotency/dedup key for exactly-once (S5) and
   the primary sort key of the total order (§4). (**D2**)

2. **Acknowledgement (`Color-Ack-Base` + `Color-Ack-New`).** Two cooperating
   fields, both **frozen at the moment the request is first generated** and
   resent unchanged on every retry:
   - **`Color-Ack-Base = b`** — the cumulative "all responses below `b`
     received" low-water mark (the *smallest id* of **D1**). It is
     non-decreasing across `seq`. It drives **buffer release** (**D4**) and
     bounds state, and it is the natural checkpoint datum for Phase II.
   - **`Color-Ack-New = [i₁, i₂, …]`** — the responses newly received since the
     previous request was generated, **in receipt order** (the *larger,
     non-contiguous ids* of **D1**, refined to be *ordered* — see §11.1). This
     is what lets the server reproduce the client's history order (**D6**).

   Together they establish the happens-before edges (S4): every response listed
   or covered by request `X` "happened before" `X` was generated.

3. **History hash (`Color-Hash`).** A rolling hash of the committed token
   sequence, piggybacked on every message, letting the peer verify — cheaply and
   incrementally — that both sides agree on the history prefix up to a named id.
   (**D5**, defined precisely in §7.)

---

## 4. The committed history and its total order (D6)

### 4.1 Definition

The **committed history** `H` is a single interleaved sequence of request tokens
`R<seq>` and response tokens `r<seq>`. Both parties construct `H` by the **same
deterministic procedure**, driven only by the per-request, frozen data
`new(X) = Color-Ack-New(X)` for `X = 1, 2, 3, …`:

```
H = []
for X = 1, 2, 3, …:                # request ids, in order
    for i in new(X):               # the frozen, receipt-ordered delta
        append r<i> to H
    append R<X> to H
```

In words: **requests are ordered by id; each response `r<i>` is placed
immediately before the first request that acknowledges it, and responses
acknowledged by the same request appear in the order the client received them.**

This is the exact realization of **D3**'s illustration
`{ …, 123, rsp-123, 234, 256, rsp-256, rsp-234, … }` and of **D6** ("client
orders responses as received; server derives response position from the
acknowledging request").

### 4.2 Why both sides compute the identical `H` (the D6 proof)

Let `new(X)` be request `X`'s frozen `Color-Ack-New` list.

**Lemma 1 (each response committed exactly once).** The client places response
`i` into `new(X)` for exactly one `X`: the id of the request it generates first
after receiving `r<i>` (receipt happens at one instant; the "next request"
is unique). Hence across all `X`, the `new(X)` lists **partition** the set of
received responses, each carrying its receipt position. ∎

**Lemma 2 (`r<i>` follows `R<i>`).** If `i ∈ new(X)` then the client received
`r<i>` before generating `X`, so it had already generated `R<i>`; since ids are
assigned in generation order, `i < X`. In the procedure `R<i>` is appended at
iteration `i` and `r<i>` at iteration `X > i`, so `r<i>` comes after `R<i>`. ∎

**Lemma 3 (both sides see the same `new(X)`).** `Color-Ack-New(X)` is frozen when
`X` is first generated and is retransmitted byte-for-byte on every retry
(§2, §5). The server commits `X` only from a delivered copy of request `X`
(§6). Therefore the server's `new(X)` equals the client's `new(X)` for every `X`
the server has committed. ∎

**Theorem (identical total order).** The construction procedure is a pure
function of the sequence `new(1), new(2), …`. By Lemma 3 both sides feed it
identical inputs (for every prefix the server has reached), and the client
always holds the full prefix by construction. Therefore the server's committed
history is always a **prefix** of the client's, and on their common prefix the
two histories are **identical**, token for token. The `Color-Hash` checks
(§7) detect at runtime any violation of this equality. ∎

The application on each side observes exactly this ordered `H` as the
"conversation state" (a requirement), and — because the server commits in id
order (§6) — always sees a consistent, gap-free ordered prefix.

---

## 5. Client behaviour

### 5.1 State

```
next_seq      = 1                 # next request id to assign
received      = {}                # set of response ids received
pending_new   = []                # responses received since last request gen,
                                  #   in receipt order (becomes next Ack-New)
base          = 1                 # 1 + longest contiguous received prefix
cur_hash      = H0                # running committed-history hash (§7)
hashmap       = {}                # token -> hash-after-that-token
inflight      = {}                # seq -> the exact bytes to (re)send
```

### 5.2 Generating a request (application wants to send payload `p`)

```
seq = next_seq;  next_seq += 1
new = pending_new;  pending_new = []          # freeze the receipt-ordered delta

# commit locally, in the canonical order (§4)
for i in new:
    append r<i> to history;  cur_hash = Hash(cur_hash, "r"+i);  hashmap[r<i>] = cur_hash
append R<seq> to history;    cur_hash = Hash(cur_hash, "R"+seq); hashmap[R<seq>] = cur_hash

base = 1 + (longest prefix 1..m all in received)

msg = POST <application-defined path>
      Color-Seq: seq, Color-Ack-Base: base, Color-Ack-New: csv(new),
      Color-Hash: cur_hash, body: p
inflight[seq] = msg                            # frozen; retries resend verbatim
transport.send(msg)                            # transport retries on loss
```

### 5.3 On receiving a response (`Color-Seq = i`, `Color-Hash = hs`, body)

```
if i not in received:            # first receipt (ignore duplicate deliveries)
    assert hs == hashmap[R<i>]   # server agrees on the prefix ending at R<i>
    received.add(i)
    pending_new.append(i)        # record receipt order for the next request
    deliver body to the application
remove i from inflight           # got an answer; stop retrying seq i
```

Duplicate deliveries of the same response are idempotent (the `if` guards them).
The client never needs to reorder: it records receipt order in `pending_new`,
and the ordering is frozen into the next request's `Color-Ack-New`.

---

## 6. Server behaviour

### 6.1 State

```
committed_upto  = 0              # highest seq with R1..seq all committed
pending_reqs    = {}             # seq -> (base,new,hash,payload) waiting for gaps
cur_hash        = H0             # running committed-history hash (§7)
hashmap         = {}             # token -> hash-after-that-token
resp_buffer     = {}             # seq -> response payload, kept until acked
acked           = {}             # response ids known received by client
processed       = {}             # seq -> committed?  (dedup for exactly-once)
```

### 6.2 On receiving a request `(seq, base, new, hash, payload)`

```
# 1. Apply the acknowledgement: release what the client has now confirmed (D4).
mark all ids < base as acked;  for i in new: mark i acked
for each id j newly acked:  drop resp_buffer[j]           # bounded buffer (L2)

# 2. Exactly-once dedup: a retry of an already-committed request is not reprocessed.
if seq in processed:
    if seq in resp_buffer:  resend resp_buffer[seq]        # lost-response recovery
    else:                   reply 200 with empty body      # already acked; no-op
    return

# 3. Stage the request and advance the committed history in strict id order.
pending_reqs[seq] = (base, new, hash, payload)
while (committed_upto + 1) in pending_reqs:
    X = committed_upto + 1
    (bX, newX, hX, pX) = pending_reqs.pop(X)
    for i in newX:
        append r<i> to history;  cur_hash = Hash(cur_hash, "r"+i);  hashmap[r<i>] = cur_hash
    append R<X> to history;      cur_hash = Hash(cur_hash, "R"+X);  hashmap[R<X>] = cur_hash
    assert hX == cur_hash            # client/server histories agree up to R<X> (§7)
    committed_upto = X
    processed[X] = committed

    resp = application.process(pX, history)   # invoked in committed order; sees conversation state
    resp_buffer[X] = resp
    send response { Color-Seq: X, Color-Hash: hashmap[R<X>], body: resp }
```

Key points:

- **Commit in id order.** A request whose predecessors have not yet arrived is
  parked in `pending_reqs` (spec §6: "the server may delay a request due to
  message ordering or gaps"). Its HTTP response is produced only once it commits.
- **The application is invoked in committed order**, so it always sees a
  gap-free ordered history prefix — the "conversation state" the requirements
  ask Color to expose.
- **Out-of-order responses at the client** arise from the network reordering the
  independent response messages (or, optionally, from an order-independent app
  choosing to answer late), not from out-of-order commits. Either way the history
  order is fixed by §4, independent of when responses are generated.
- **Exactly-once (S5).** `processed` guarantees `application.process` runs at
  most once per `seq`. A retried request returns the *buffered* response, never a
  recomputation.

---

## 7. History hashing (D5), precisely

Define a rolling hash over the committed token stream:

```
H0        = SHA256("color/v1")                      # fixed seed
Hash(h,t) = SHA256( h  ||  ":"  ||  t )              # t is "R<n>" or "r<n>"
```

After committing the `k`-th token `t_k`, the running hash is
`h_k = Hash(h_{k-1}, t_k)`, and each side stores `hashmap[t_k] = h_k`. So
`hashmap[R<n>]` is the hash of the entire committed history up to and including
`R<n>`, and likewise for `r<n>`.

**What each message carries and how it is checked:**

- A **request** `X` carries `Color-Hash = hashmap[R<X>]` (client side). When the
  server commits `R<X>` it recomputes the same value and asserts equality
  (§6, step 3). This verifies the two histories are identical through `R<X>`,
  *including* every `r<i>` in `new(X)` that was just committed before it — so
  response ordering is verified here too.
- A **response** for `X` carries `Color-Hash = hashmap[R<X>]` (server side). The
  client compares it against its own `hashmap[R<X>]` on receipt (§5). This
  confirms the server reached the same prefix the client did at `R<X>`.

A response cannot carry a hash that includes its own `r<X>` token, because at
response-send time `r<X>` is not yet committed on either side (it will be, at the
future request that acknowledges it — §4). Its position/ordering is instead
verified when that acknowledging request round-trips. Every token is therefore
covered by exactly one hash check, transitively guaranteeing full-history
agreement (the §4.2 theorem, checked at runtime).

The verification harness (per requirements) uses exactly this: keep the
`{ id → hash }` map on both sides and compare the piggybacked hash against the
local map entry for the named id; any mismatch is a safety violation and fails
the run.

> Illustrative note: the worked examples in §8 abbreviate hashes to short
> symbolic tags (`h1`, `h2`, …) rather than real 256-bit digests. Equal tags
> mean "the two sides computed the same hash for the same prefix."

---

## 8. Worked examples (HTTP wire dumps)

Ids are shown small and contiguous (`1, 2, 3, …`); "gaps" appear only in which
*responses* have been received. Hash tags are symbolic (see the note above);
the running history and its hash tags are:

```
token:  R1   r1   R2   R3   r3   r2   R4
hash:   h1   h2   h3   h4   h5   h6   h7      # h_k = Hash(h_{k-1}, token_k), h0 = H0
```

### 8.1 Simple sequential exchange

Client sends request 1 (nothing received yet, so `Ack-Base: 1`, empty
`Ack-New`), commits `R1` → hash `h1`:

```
POST … (application-defined path)
Color-Seq: 1
Color-Ack-Base: 1
Color-Ack-New:
Color-Hash: h1
Content-Type: application/json

{"ts":"2026-07-11T18:00:00.000Z"}
```

Server commits `R1` (no gap), recomputes `h1` ✓, runs the app, buffers and
returns the response echoing `seq` and carrying `h1`:

```
200 OK
Color-Seq: 1
Color-Hash: h1
Content-Type: application/json

{"ts":"2026-07-11T18:00:00.004Z"}
```

Client receives `r1` (checks `h1` ✓), records it in `pending_new`. Next request
2 acknowledges it: `Ack-Base: 2`, `Ack-New: 1`, committing `r1` then `R2`
→ hash `h3`:

```
POST … (application-defined path)
Color-Seq: 2
Color-Ack-Base: 2
Color-Ack-New: 1
Color-Hash: h3
...
```

On receiving request 2 the server commits `r1, R2` (recomputes `h2, h3` ✓) and,
seeing `1` acknowledged, **drops `r1` from its response buffer** (D4).

### 8.2 Parallel requests, out-of-order responses (the D3 example)

Client has already exchanged request 1 (`r1` received). It now issues requests
**2 and 3 in parallel**, before either response returns:

```
POST … Color-Seq: 2  Color-Ack-Base: 2  Color-Ack-New: 1   Color-Hash: h3
POST … Color-Seq: 3  Color-Ack-Base: 2  Color-Ack-New:     Color-Hash: h4
```

(request 3 carries empty `Ack-New` — no new response arrived between generating 2
and 3.) The server commits `r1,R2` then `R3`, generates responses 2 and 3 in id
order, and sends both — but **the network delivers response 3 before response
2**:

```
200 OK   Color-Seq: 3   Color-Hash: h4     # arrives first
200 OK   Color-Seq: 2   Color-Hash: h3     # arrives second
```

The client's receipt order is therefore `r3` then `r2`, so `pending_new = [3, 2]`.
Request 4 freezes that order and drives base to 4 (all of 1,2,3 received):

```
POST … (application-defined path)
Color-Seq: 4
Color-Ack-Base: 4
Color-Ack-New: 3,2
Color-Hash: h7
...
```

Both sides now commit `r3, r2, R4` (in that receipt order), yielding the history

```
R1  r1  R2  R3  r3  r2  R4
```

i.e. `{ 1, rsp-1, 2, 3, rsp-3, rsp-2, 4 }` — exactly the **D3** shape, receive
order (`rsp-3` before `rsp-2`) preserved, and the server drops `r2, r3` from its
buffer. This is the case a cumulative-set-only ack could **not** have ordered
correctly; the ordered `Color-Ack-New` is what makes it work (§11.1).

### 8.3 Dropped request → transport retry (client-side injection)

The libcurl failure-injection wrapper drops the *outgoing* request 2. The
transport retries the **identical** POST (same `seq`, `base`, `new`, `hash`,
body). The server sees request 2 for the first time on the retry and processes
it exactly once. If instead the wrapper had dropped it after the server already
committed it, step 2 of §6.2 (`seq in processed`) resends the buffered response
without reprocessing.

### 8.4 Dropped response → exactly-once recovery

Server commits request 2, runs the app **once**, buffers `r2`, and sends it — but
the wrapper drops the *incoming* response. The client, still lacking a response
for `seq 2`, retries the identical request 2. The server finds `2 ∈ processed`
with `r2` still buffered and **resends the buffered `r2`** (no reprocessing):

```
# retry (identical bytes)
POST … Color-Seq: 2  Color-Ack-Base: 2  Color-Ack-New: 1  Color-Hash: h3

# server response (resent from buffer, not recomputed)
200 OK  Color-Seq: 2  Color-Hash: h3
{"ts":"2026-07-11T18:00:00.004Z"}        # identical payload as first generation
```

This is S5 in action: "a request should not be re-processed to recover a lost
response."

### 8.5 Buffer release

Server-side `resp_buffer` holds `r<i>` only until `i` is acknowledged. When a
request arrives with `Ack-Base = b`, every buffered `r<i>` with `i < b` is
released; every `i` in `Ack-New` is released too. In §8.2, request 4
(`Ack-Base: 4`) releases any still-buffered `r1, r2, r3` at once. Continued
client progress therefore keeps the buffer bounded (L2, §10).

### 8.6 Hash mismatch = detected safety violation

Suppose (hypothetically, e.g. a protocol bug) the server committed `r2` before
`r3`. On committing `R4` it would compute a hash `h7' ≠ h7` and the assertion in
§6.2 step 3 fails immediately: the request's `Color-Hash: h7` disagrees with the
server's recomputed value. The verification harness flags the run as a safety
violation at the exact divergent token. This is how §4.2's guarantee is policed
at runtime.

---

## 9. Safety argument (mapping to spec.md)

| Property | How Color satisfies it |
|---|---|
| **S1 ordered requests** | Contiguous monotonic `Color-Seq`; the total order (§4) uses request id as primary key. |
| **S2 every request answered, retriable** | Client retries the identical POST until a response for its `seq` is received; server always produces (or re-sends) a response for a committed request. |
| **S3 symmetric receipt** | The client acknowledges `r<i>` in the next request's `Ack-New`/`Ack-Base`; the server learns of receipt only through that ack and buffers `r<i>` until then, so "client received `r<i>`" ⇔ "server knows it". |
| **S4 causal acknowledgement** | `Ack-Base`+`Ack-New` (frozen at generation) record exactly the responses that happened-before the request; §4 places them before that request in `H`. |
| **S5 exactly-once** | `processed` dedups by `seq`; retries return the buffered response; the app runs at most once per `seq`. |
| **Invariant: single identical total order** | §4.2 theorem — both sides compute `H` from the same frozen `new(X)` inputs; server history is always a prefix of the client's and identical on the common prefix; `Color-Hash` checks (§7) detect any divergence at runtime. |

---

## 10. Liveness and bounded buffers

- **L1 progress under loss.** Any dropped request or response triggers an
  immediate transport retry of the identical POST. Under fair loss (not every
  copy dropped forever) each request is eventually delivered and committed in id
  order, and each response eventually delivered, so the conversation advances.
- **L2 bounded buffers.** Three quantities must stay bounded:
  - *Server response buffer* — `r<i>` is released as soon as `i` is
    acknowledged, which happens on the very next request the client sends after
    receiving `r<i>`. So the buffer holds at most the responses received-but-not-
    yet-acknowledged, i.e. bounded by the client's outstanding window.
  - *Server pending-request staging* — `pending_reqs` holds only requests that
    arrived ahead of a gap; bounded by the same in-flight window.
  - *Ack size* — `Color-Ack-New` lists only responses received since the
    previous request; bounded by responses-per-inter-request interval.

  All three are bounded provided the client does not run unboundedly far ahead of
  its acknowledgements — a standard flow-control (bounded-window) assumption. The
  verification harness measures these to confirm the "retransmission buffer
  growth is limited" requirement.

---

## 11. Design decisions and notes

### 11.1 Receive-order refinement of the acknowledgement (the D6 crux)

Plan decision **D1** described the acknowledgement as "the smallest id (all
earlier received) plus larger non-contiguous received ids". Taken literally as a
*cumulative set*, that cannot convey the client's **receive order** among
responses acknowledged together — yet the **D3** example explicitly commits
`rsp-256` before `rsp-234` (completion order, not id order). A set-difference on
the server would instead order them by id and diverge from the client.

Resolution (this design): split the acknowledgement into

- **`Color-Ack-Base`** — the cumulative low-water set (D1's "smallest id"),
  used for buffer release and Phase II checkpointing; and
- **`Color-Ack-New`** — the responses received *since the previous request*,
  **in receipt order** (D1's "larger non-contiguous ids", refined to be
  ordered).

`Color-Ack-New` is the ordering channel; `Color-Ack-Base` is the release
channel. `Color-Ack-Base` alone can lose order (responses that fall below the
low-water before any request commits them), which is why the ordered delta is
required. This is the single point where the design exceeds the literal D1
wording, and it is what makes the §4.2 proof — and the D3 example — hold.
**Please confirm this refinement on review.**

### 11.2 In-order commit; out-of-order responses come from the network

The server commits history strictly in id order and invokes the application in
that order, so the app always sees a consistent conversation prefix. Responses
still reach the client out of order because the independent HTTP responses race
on the wire. An application that is order-independent could be allowed to
generate responses out of id order without affecting history correctness (the
order in `H` is fixed by §4 regardless); Phase I keeps the simple in-order model.

### 11.3 Frozen requests

A request's headers (`Seq`, `Ack-Base`, `Ack-New`, `Hash`) and body are fixed
when it is first generated and are byte-identical on every retry. This immutability
is what makes retries idempotent and makes the server's and client's `new(X)`
provably equal (Lemma 3).

### 11.4 Duplicate-after-ack

If a long-delayed duplicate of an already-acknowledged request arrives after its
response buffer was released, the server replies `200` with an empty body (§6.2
step 2). The client will not actually be waiting on it (it already received and
acknowledged that response), so the reply is harmlessly discarded.

---

## 12. Forward pointer to Phase II (failover)

Phase II (`docs/failover.md`) will checkpoint the server's committed history plus
`resp_buffer` and `acked`/`committed_upto` (note `Color-Ack-Base` is already the
compact form of "what the client has confirmed"). On failover a new server loads
the checkpoint; a request whose history the new server lacks is answered with a
`5xx`, prompting the client to replay its known request/response history in a
single JSON-enveloped request so the new server can rebuild. None of this changes
the Phase I wire rules above; it only adds recovery on top of them.
