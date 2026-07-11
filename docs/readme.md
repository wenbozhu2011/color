# Color documentation

Two kinds of documents live here.

## Provided (authored by the project owner)

These are the inputs that define the task; they are the source of truth for
*what* Color must be.

- **[spec.md](spec.md)** — the problem definition and the communication
  properties Color must satisfy.
- **[requirements.md](requirements.md)** — the implementation requirements
  (components, verification, demo, and the two phases).

## Generated (in [`claude/`](claude/))

The documents under `claude/` are the **planning and design artifacts generated
by Claude and reviewed/approved by the project owner** while building Color. They
elaborate the provided spec and requirements into a concrete design and record
the decisions made along the way.

- **[claude/plan.md](claude/plan.md)** — the shared-understanding plan: how the
  spec/requirements were interpreted, the design decisions, and the milestones.
- **[claude/protocol.md](claude/protocol.md)** — the Phase I REST protocol
  design (wire format, headers, ordering rule, and the safety proof).
- **[claude/failover.md](claude/failover.md)** — the Phase II failover protocol
  (checkpointing and the `503`/replay recovery).
- **[claude/verification.md](claude/verification.md)** — the verification
  harness design: what is checked and how the properties are proved.

For an end-user overview and how the pieces fit together, see the top-level
[README](../README.md).
