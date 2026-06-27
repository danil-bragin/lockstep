# Lockstep Query Model & Developer Surface

This is the external-facing guide to Lockstep's **KV / typed-query** developer
surface: how you author transactions, how the read/query model works, what the
four consistency levels mean and how they are made visible at every call site, the
wire protocol, and how to use the reference driver and the CLI. It covers the
Phase-6 foundation (C6.1–C6.6).

> **SQL is a layer above this.** A from-scratch SQL engine
> (`query/include/lockstep/query/sql/`) was built on top of the `Database` /
> `TxnFn` model documented here — 30+ features (joins, aggregates, GROUP BY/HAVING,
> subqueries, window fns, constraints, indexes, a rich type system, row + columnar
> execution) plus **distributed SQL** (`DistributedSql.hpp`, co-located-shuffle
> star-JOIN pushdown). This doc describes the substrate the SQL layer compiles down
> to; for the SQL feature set see `query/sql/SQL_FEATURES_PLAN.md`.

Lockstep is a C++23 **deterministic** distributed database. Everything below is a
pure function of its inputs: given the same seed and the same operations, you get
byte-identical results. There is no wall-clock, no ambient randomness, and no
hidden nondeterminism anywhere in the surface.

---

## 1. The transaction-function model (one-shot, deterministic)

A Lockstep transaction is a **one-shot function**, not an interactive session.
You do not `BEGIN`, read, think, write, and `COMMIT` over several round trips.
Instead you author a single deterministic function: parameters go in, a result
and a set of writes come out. The whole function is submitted, sequenced into one
global total order, and executed deterministically in that order. The sequencer
order *is* the serialization order — which is what makes the default path
strict-serializable.

You author a transaction as a `TxnFn`:

- **`declared`** — the read footprint: the keys the body predicts it will read,
  each carrying its consistency level.
- **`body`** — a deterministic command over a `TxnContext`.

```cpp
using namespace lockstep::query;

TxnFn transfer;
transfer.id = 1;                                   // a stable client tag
transfer.declared = reads(declare::strict("acct:a"),
                          declare::strict("acct:b"));
transfer.body = [](TxnContext& ctx) {
    auto a = ctx.read("acct:a");                   // the value the executor gave
    auto b = ctx.read("acct:b");
    // ... compute new balances from a and b ...
    ctx.write("acct:a", new_a);
    ctx.write("acct:b", new_b);
    ctx.result("transferred");                     // an observable result token
};
```

### Why the body cannot be nondeterministic (V-DET-USER)

The body receives **only** a `TxnContext&`. That handle exposes exactly four
operations:

| call | meaning |
|---|---|
| `ctx.read(key)` | the value the executor presented for a declared key (∅ if absent) |
| `ctx.write(key, value)` | stage a write to commit (last write of a key wins) |
| `ctx.also_read(key)` | surface a value-dependent read the body discovered it also needs |
| `ctx.result(token)` | set the observable result the client receives |

There is **no clock, no random source, and no I/O handle** on `TxnContext`. A body
that tried to read the time or roll a die simply has nothing to call — so a
nondeterministic transaction *does not compile*. This is the V-DET-USER guarantee,
and it is enforced by the type, not by convention.

### Value-dependent reads (OLLP)

If, based on what it read, the body decides it needs a key it did *not* declare,
it calls `ctx.also_read(key)`. The executor detects the footprint mismatch and
**re-sequences** the transaction with a fresh snapshot (bounded by a retry limit;
past the bound it deterministically aborts). This is the honest "optimistic
lock-location prediction" path — a committed transaction always read exactly the
keys it declared, at its own serialization point.

---

## 2. The read/query model (typed, composable, non-SQL)

Reads are not SQL strings. A query is a small value: a sequence of typed **read
steps** over an opaque byte key space. There are two kinds of step:

- **point** — `get(key)`
- **range** — `scan(lo, hi)` (half-open `[lo, hi)`) or `scan_from(lo)` (open-ended)

You compose them on a `Query<L>` builder, where `L` is the consistency level (see
§3). The composition *is* the plan: a planner maps the steps to versioned MVCC
reads at one chosen committed snapshot. There is no parser and no expression
engine.

```cpp
Query<Strict> q;
q.get("acct:a").get("acct:b").scan("idx:", "idx;");   // three steps, strict level
```

A query result gives you, per step, the value(s) observed plus the committed
prefix it was served as-of.

---

## 3. The four consistency levels (D5), call-site-visible

Every read carries a consistency level. The level is **part of the type**, so it
is impossible to omit or silently misuse (this is the V-D5-SAFE invariant). If you
forget to name a level, you get the *strongest* contract — never an accidental
stale read.

| level | meaning | real-time? |
|---|---|---|
| **Strict** (default) | linearizable: sees the committed prefix strictly before this read | yes |
| **Snapshot(version)** | consistent as-of one committed version, no torn read | no |
| **Bounded(max_lag)** | a local-replica read at most `max_lag` committed entries behind the tip | no |
| **RYW(session)** | within a session, observe that session's own prior committed writes | within session |

```cpp
Query<Strict>()                    // linearizable, the strong default
Query<Snapshot>(Snapshot{v})       // as-of version v (no torn read)
Query<Bounded>(Bounded{k})         // within k entries of the tip
Query<RYW>(RYW{session})           // see my session's writes
```

Because the level rides on every read, **the same per-level checkers judge a query
read exactly as they judge a transaction read.** A Snapshot read as-of an older
version legitimately returns the older value; a Strict read of the same key
returns the latest committed value. The level you wrote at the call site is the
contract you get — no stronger, no weaker.

---

## 4. The wire protocol (summary)

The client talks to the server over a hand-rolled, length-prefixed, CRC-checked
binary protocol. Postgres-wire compatibility is explicitly deferred.

**Frame shape:** `[u8 msg_kind] <body> [u32 crc32]`. Every integer is fixed-width
little-endian; every variable field is length-prefixed; a trailing CRC-32 covers
the whole body. On decode the CRC is re-derived and the frame is **rejected** on
mismatch — a torn, truncated, or bit-rotted frame is a clean decode failure, never
a mis-decoded or fabricated message. A bad frame is dropped and the sender retries.

**Requests:** `Ping`, `Submit`, `Query`, `SqlExec` (a SQL statement string — the
SQL-over-wire path).
**Responses:** `Pong`, `SubmitOk`, `QueryOk`, `SqlResult` (rows blob), `Error`.

`Submit` never ships executable code (that would be neither encodable nor safe).
It names one of a fixed catalogue of **deterministic ops** — `Put`, `Transfer`,
`Increment` — plus its parameters, and the server materializes the matching pure
body. This keeps V-DET-USER intact across the wire.

### Exactly-once

`SimNetwork` may duplicate, reorder, and drop frames. A dropped reply makes the
client retry. Each `Submit` carries an idempotent **submit key**; the server keeps
a dedup table keyed by it. The first time a submit key is seen, the transaction is
applied and its response memoized; every later delivery of that key returns the
memoized response **without re-applying**. So a re-delivered or retried submit
produces no duplicate effect — the submit key *is* the idempotent transfer id.

---

## 5. The reference driver (C6.4)

The driver is the ergonomic high-level client you actually program against. It
wraps the low-level client stub and hides all the protocol plumbing — request ids,
the submit-key lifecycle, the timeout/retry loop, the background reply pump. You
just call verbs and get value-shaped outcomes back. **Exactly-once is automatic:**
the driver allocates exactly one submit key per logical call and reuses it across
every transport retry of that call.

```cpp
using namespace lockstep::query;

// `conn` is a Connection bound to a ClientStub (see §7 for the in-process setup).
WriteOutcome w;
co_await conn.put("acct:alice", "100", w);
co_await conn.transfer("acct:alice", "acct:bob", 30, w);   // exactly-once
co_await conn.increment("counter", 1, w);

ReadOutcome r;
co_await conn.get("acct:alice", r);                        // strict (default)
co_await conn.get_snapshot("acct:alice", /*version=*/1, r);
co_await conn.get_bounded("acct:alice", /*max_lag=*/5, r);
co_await conn.get_ryw("acct:alice", /*session=*/9, r);
co_await conn.scan("acct:a", "acct:z", r);

PingOutcome p;
co_await conn.ping(p);
```

Outcomes are plain values — no wire types leak through:

- `WriteOutcome`: `ok`, `committed`, `attempts`, `commit_version`, `result`, `writes`.
- `ReadOutcome`: `ok`, `attempts`, `level`, `served_version`, `rows` (point reads),
  `ranges` (scans).
- `PingOutcome`: `ok`, `attempts`.

`ok == false` means the call timed out after the retry budget. `attempts` tells you
how many sends it took — useful for observing the retry path under faults.

This C++ driver is the *reference* shape. A driver in another language mirrors the
same surface: open a connection, call a verb, get a typed result or error, with
exactly-once owned by the driver.

---

## 6. The CLI (C6.5)

`lockstep_cli` is a small command-driven admin/client tool. It parses a command
list (from `argv` after a `--`, or one-per-line on stdin), drives the reference
driver against an in-process sim server, and prints each command's result. It is
fully deterministic: same `--seed` plus same script yields byte-identical output.

```
lockstep_cli --seed 7 -- \
    put acct:a 100 \
    transfer acct:a acct:b 30 \
    get acct:a \
    get acct:b --level snapshot --arg 1 \
    scan acct:a acct:z \
    ping
```

**Commands:**

| command | effect |
|---|---|
| `put <k> <v>` | put `k = v` |
| `transfer <a> <b> <amt>` | move `amt` from `a` to `b` (exactly-once) |
| `increment <k> <delta>` | `k += delta` |
| `get <k> [--level L] [--arg n]` | point read at level `L` |
| `scan <lo> <hi> [--level L] [--arg n]` | range read `[lo, hi)` at level `L` |
| `ping` | liveness probe |
| `# ...` | a comment line (ignored, stdin mode) |

`L` is one of `strict` (default), `snapshot`, `bounded`, `ryw`; `--arg` supplies the
level's parameter (snapshot version / bounded max-lag / ryw session).

**Global flags:** `--seed <n>` (default 1), `--faults` (turn on a dup/reorder/drop
net profile — still exactly-once and deterministic), `--trace` (also print the
deterministic scheduler event trace).

All commands run against one server state in one deterministic run, so reads see
prior writes.

---

## 7. Putting it together (the in-process cluster)

`LocalCluster` stands up the full stack — SimNetwork bus → server → Database → txn
executor → MVCC store, plus a client stub the driver wraps — on one seeded
scheduler. You hand it a "driver program" (a coroutine over a `Connection`):

```cpp
LocalCluster lc(/*seed=*/42);
lc.run([](Connection& conn) -> core::Task {
    WriteOutcome w;
    co_await conn.put("k", "v", w);
    ReadOutcome r;
    co_await conn.get("k", r);
    co_return;
});
// lc.trace() is byte-identical for the same (seed, faults, program).
```

A complete worked example lives in `cli/bank_example.cpp`: it seeds two accounts,
moves money, and reads balances at strict and snapshot levels — showing exactly
what each level returns and why.

---

## 8. Conformance

The whole surface is held to the strict-serializable oracle. The conformance gate
(`tests/query_conformance_test.cpp`) drives a transfer workload **all the way
through** the driver → wire → server → Database → executor → MVCC store, over a
seed sweep under network faults, and asserts:

- end-to-end results equal the strict-serializable oracle on the default path
  (clean *and* under dup/reorder/drop);
- exactly-once holds end to end (no duplicated money move);
- each consistency level honors exactly its contract, judged by the matching
  per-level checker;
- determinism: same seed yields byte-identical results.

If a developer-surface change broke any of these, the gate fails. Nothing in the
surface is trusted unless it agrees with the ground truth.
