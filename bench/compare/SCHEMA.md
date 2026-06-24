# Common result schema — every adapter emits ONE JSON object per cell to results/<cell-id>.json

A "cell" = one (system × vector × workload × cpus × nodes × concurrency × pass). The
orchestrator skips a cell whose JSON already exists (resumable). report.py reads all
results/*.json into the comparison.

```json
{
  "cell_id":   "lockstep__kv__write__cpus4__nodes1__shards1__conc64__p0",
  "system":    "lockstep | postgres | etcd | cockroach | tikv",
  "vector":    "kv | scaling | fault | sql",
  "workload":  "write | rw5050 | read | tpcc",
  "cpus":      4,                 // server --cpus pin (the swept axis for vector=scaling)
  "mem_g":     6,
  "nodes":     1,                 // 1 single-node, 3 replicated (fault vector)
  "shards":    1,                 // lockstep multi-shard count; 1 for others
  "concurrency": 64,              // client threads / pipeline inflight (matched across systems per cell)
  "value_bytes": 16,
  "op_count":  100000,            // total ops in the measured window
  "pass":      0,                 // 0..N-1 fresh repeat; report takes the MEDIAN

  "throughput_ops_s": 17250.0,    // HEADLINE = COMMITTED ops/s (NOT accept/append). null on failure.
  "p50_us":    540.0,             // nearest-rank; null if the tool gives no latency
  "p99_us":    1900.0,

  "fault": {                      // populated only for vector=fault
    "injected":     true,
    "recovery_ms":  820.0,        // time from leader-kill to next committed op
    "acked_lost":   0,            // acked writes missing after recovery (MUST be 0 for a consistent system)
    "consistent":   true          // post-recovery state passed the consistency check
  },

  "ok":    true,                  // cell produced a usable measurement
  "raw":   "PBENCH count=... commit_tput=17250.0 ...",   // verbatim tool output line(s) for audit
  "notes": "lockstep native pbench; commit_covered=1"
}
```

## Rules every adapter MUST honor

- **throughput_ops_s is COMMITTED, never accept/append.** Lockstep: `commit_tput` (require
  `commit_covered=1`, else `ok=false`). etcd: quorum-acked puts. Postgres: COMMIT returned.
  Cockroach/TiKV: committed txn. A tool that only reports an un-committed rate ⇒ adapter must
  derive committed or set `ok=false` with a note.
- **Identical workload knobs** (`value_bytes`, `op_count`, `concurrency`, keyspace) come from the
  orchestrator env and are echoed into the JSON — they are equal across systems for a given cell.
- **`raw` is mandatory** — the verbatim tool line, so any number is auditable back to its source.
- **Failure is explicit**: a crash / no-leader / uncovered-commit ⇒ `ok=false`, `throughput_ops_s=null`,
  a `notes` saying why. Never fabricate a number; report.py drops `ok=false` cells and lists them.
- **Resource pin is identical**: the SAME `--cpus`/`--memory` is applied to the server container
  for every system in a cell. The client/driver runs with separate, fixed headroom; if the client
  is suspected to cap throughput (e.g. single-thread past high shard counts), say so in `notes`.
```
