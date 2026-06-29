# Distributed star-JOIN — co-located shuffle vs gather-the-fact

Does Lockstep's co-located-shuffle star-JOIN pushdown actually beat the naive
"gather the fact to the coordinator and join there" baseline? This measures the
**same shards, same data, same query, both ways** — the only difference is the
`pushdown_enabled` A/B knob on `DistributedSql`.

- **Pushdown** aggregates the large FACT *by the join key on each shard* and ships
  only the per-key partials (cardinality = distinct keys); the fact is **never
  gathered**. The tiny dim is gathered once and the partials roll up to the dim
  groups.
- **Gather** (the baseline) pulls *every* fact row to the coordinator, rebuilds
  the table in a local engine, and runs the join there.

Both return a **byte-identical** result — this is a pure performance comparison.

## Setup

`bench/compare/distributed_join/dist_join_bench.cpp` — 8 in-process shards (columnar),
a FACT of N rows sharded by PK, a DIM of 1000 keys mapped to 8 group labels:

```sql
SELECT dim.label, COUNT(*), SUM(fact.amt)
FROM fact JOIN dim ON fact.fk = dim.k
GROUP BY dim.label
```

Run (on a developer laptop; relative numbers, not a production benchmark):

```sh
bash bench/compare/distributed_join/run.sh            # default N=1,000,000
bash bench/compare/distributed_join/run.sh 200000     # smaller
```

## Results

| FACT rows | pushdown | gather (baseline) | **speedup** | data to coordinator (pushdown vs gather) |
|-----------|---------:|------------------:|------------:|------------------------------------------|
| 200,000   | 4.0 ms   | 1,684 ms          | **424×**    | ~8,000 partials vs 200,000 rows (25×)    |
| 1,000,000 | 11.3 ms  | 27,121 ms         | **2,402×**  | ~8,000 partials vs 1,000,000 rows (125×) |

The gap **widens with scale**: the pushdown cost tracks the join-key cardinality
(constant ~8,000 partials here), while the gather cost tracks the fact size (it
re-materializes every row at the coordinator — ~27 s for 1 M rows). The result is
byte-identical both ways, so the speedup is free correctness-wise.

> Relative numbers on one laptop; the **shape** (pushdown flat in fact size, gather
> linear; result identical) is the result. The "data to coordinator" column is the
> structural point: the fact never crosses the network under the pushdown.
