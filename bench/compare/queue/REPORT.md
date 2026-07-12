# K3.4 — Lockstep queues vs pgmq (2026-07-12)

Harness: `run_queue.sh` — Lockstep embedded (libc++ Release, cpu-pinned) vs
quay.io/tembo/pg17-pgmq (fsync off, same pin), pgmq ops SERVER-SIDE (its best case:
generate_series send loop, DO-block read+delete drain). N=10k, batch 100.

| op | Lockstep | pgmq | verdict |
|---|---|---|---|
| send, one statement per msg | **51-55k msg/s** | 43.0k | **Lockstep 1.3x** (and ours pays a full SQL parse per message; his loop is server-side) |
| send batch (one txn / send_batch) | 24.1k | **62.1k** | pgmq 2.6x — see the known issue below |
| receive+ack drain (batch 100) | **46.1k msg/s** | 44.4k | **Lockstep** (after the batched `ACK q, id, id, ...` — one commit per batch) |

Semantics comparison (what the numbers do not show):
- Lockstep SEND inside BEGIN..COMMIT is atomic with data writes (the outbox); pgmq's
  send is transactional with data too (same PG txn) — parity there.
- Redelivery: pgmq visibility = wall-clock (single primary decides); Lockstep = Seq
  units — deterministic and REPLICATED (every replica makes identical redelivery
  decisions; the queue survives leader failover with exactly-once intact). pgmq on a
  replica is read-only; failover semantics are Patroni's problem.
- Exactly-once gate: Lockstep ships a competing-consumer test where every message is
  ACKed exactly once; pgmq documents at-least-once.
- DLQ: both (ours auto at 5 deliveries; pgmq via archive/manual).

KNOWN ISSUE (honest): our transactional batch send (10k SENDs in one txn) is SLOWER
than 10k autocommit sends — suspected O(n^2) overlay scanning in the txn write buffer
(dup-PK/read-your-writes checks walk txn_writes_ linearly). Filed for a profile-first
pass; until then use autocommit sends for bulk enqueue.
