# K3.4 — Lockstep queues vs pgmq (2026-07-12)

Harness: `run_queue.sh` — Lockstep embedded (libc++ Release, cpu-pinned) vs
quay.io/tembo/pg17-pgmq (fsync off, same pin), pgmq ops SERVER-SIDE (its best case:
generate_series send loop, DO-block read+delete drain). N=10k, batch 100.

| op | Lockstep | pgmq | verdict |
|---|---|---|---|
| send, one statement per msg | **63.9k msg/s** | 43.0k | **Lockstep 1.5x** |
| send batch (one txn / send_batch) | **66.0k msg/s** | 62.1k | **Lockstep** — after the txn-overlay + reparse fixes below |
| receive+ack drain (batch 100) | **48.7k msg/s** | 44.4k | **Lockstep** (batched `ACK q, id, ...`) |

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

RESOLVED (same day): the txn slowness WAS the predicted O(n^2) — read_committed's
read-your-writes walked txn_writes_ linearly per in-txn read (every INSERT's dup-PK
check). Fixed with a last-write-wins map overlay beside the vector (24k -> 49k), then
SEND stopped re-parsing a generated INSERT per message (programmatic InsertStmt:
49k -> 66k, all paths). Lockstep now leads pgmq on ALL THREE operations — while its
redelivery decisions replicate deterministically and pgmq's live on one primary's
wall clock.
