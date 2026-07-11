# Verification Harness — Plan and Implementation Notes

This is the Phase I self-contained prototype (milestone 3 of `docs/plan.md` §6):
it runs the transport-agnostic **Color core** over a **simulated lossy network**
and asserts the safety/liveness properties of `spec.md` / `docs/protocol.md` on
seeded, reproducible runs. No external dependencies — pure C++17 + CMake — so it
builds and runs anywhere. The same core later runs over the real
libcurl ↔ net_http transport (milestones 4–5).

## Layout

```
core/                       transport-agnostic Color core (compiled library)
  color_message.h             Request / Response wire structs (the header set)
  color_history.h/.cc         committed history + rolling hash (§4, §7)
  color_client.h/.cc          ColorClient state machine (§5)
  color_server.h/.cc          ColorServer state machine (§6)
verification/
  plan.md                     this document
  src/
    sim_network.h             SimLink: seeded drop / duplicate / delay / reorder
    checker.h                 invariant checker (safety + liveness + bounds)
    fuzz_main.cpp             the fuzzy driver + CLI + multi-seed loop
```

## What the harness does

For each seed it constructs a fresh `ColorClient`, `ColorServer`, and two
`SimLink`s (client→server, server→client), then runs a discrete virtual-time
loop:

1. **Generate** — keep up to `--parallel` requests outstanding; each request
   body is a timestamp + nonce (the "fuzzy" payload from `requirements.md`); the
   server replies with its own timestamp.
2. **Retransmit** — any outstanding request past its `--rto` is resent
   *byte-identically* (the transport-level retry; loss stands in for the real
   client-side libcurl failure injection).
3. **Deliver requests** — `SimLink` hands due requests to the server, which may
   drop, duplicate, delay, and thereby reorder them; responses go back over the
   return link the same way.
4. **Deliver responses** — the client records receipt order and acknowledges.

After a generation phase it enters a **drain** phase (stop generating, keep
retransmitting) until every request is answered and both links are empty, so
liveness can be asserted.

## Properties checked (`checker.h`)

| Check | Property | How |
|---|---|---|
| **Safety** | single identical total order (S1–S4 + invariant, §4.2) | server's committed history must be an **exact prefix** of the client's, token-for-token |
| **Hash** | runtime history agreement (§7) | no piggybacked `Color-Hash` ever disagreed during the run |
| **Exactly-once** | S5 | the application was invoked **exactly once** per committed `seq` (despite drops & duplicates) |
| **Liveness** | L1 | after drain, every issued request is answered and the server committed all of them |
| **Bounded** | L2 | max response buffer, request staging, and `ack_new` size all stay within a window-derived bound (independent of run length) |

The checker is independently validated by a **negative test**: injecting an
ordering bug (server sorting `ack_new`, breaking the D6 receipt order) makes it
report history divergence at the exact token plus hash mismatches — confirming
the checks have teeth.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/verification/color_verify                 # 100 seeds, default settings
ctest --test-dir build --output-on-failure        # smoke + high-loss suites
```

Exit code is non-zero if any run fails, so it is CI-friendly.

### Useful flags

```
--seeds N        number of seeds (default 100)
--base-seed S    first seed (runs S..S+N-1; fully reproducible)
--steps K        generation-phase length in ticks (default 2000)
--parallel P     max outstanding requests (default 8)
--drop f         per-copy drop probability (default 0.30)
--dup f          per-delivery duplication probability (default 0.10)
--max-latency L  max per-hop delay in ticks; spread => reordering (default 5)
--rto R          retransmit timeout in ticks (default 6)
--minutes M      run seeds until M minutes of wall-clock elapse (requirements
                 ask for up to ~5 min co-located)
--verbose        print a per-message event trace (also the basis of the demo)
```

## Representative results

- `--seeds 100 --steps 2000` (defaults): 100/100 pass, ~98k requests issued
  through ~154k drops and ~36k duplicates; worst-case buffers 8 / 7 / 7.
- `--drop 0.6 --dup 0.4 --parallel 16 --max-latency 12`: 50/50 pass; buffers
  stay bounded (≤16).
- `--drop 0.8 --dup 0.5`: 30/30 pass — heavy loss just means more retransmission,
  never a safety violation.

## Notes / deviations

- **Hash function.** `docs/protocol.md` §7 specifies SHA-256 chaining; the
  prototype uses a 64-bit FNV-1a-based chain of identical *structure*
  (`h_k = Hash(h_{k-1}, token_k)`) to stay dependency-free. Because the checker
  also deep-compares the full histories, the hash is a fast early detector, not
  the sole authority — so the stand-in is safe. Swapping in SHA-256 is a
  drop-in change in `history.h`.
- **Virtual time.** Runs use a discrete tick clock for determinism and speed;
  the real-transport demo (milestone 5) uses wall-clock and a slowed rate. Use
  `--minutes` for a wall-clock-bounded soak.
