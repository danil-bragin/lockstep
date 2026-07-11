# K1.5 — vector k-NN vs pgvector (PRELIMINARY, 2026-07-11)

Harness: `run_vector.sh [N] [DIM] [K] [QUERIES] [LISTS] [PROBES]` — identical deterministic
data (shared integer formula), both systems pinned to one CPU, Docker. Lockstep embedded
(`lockstep_vector.cpp`, libc++ Release); pgvector = `pgvector/pgvector:pg16`, fsync off,
EXPLAIN ANALYZE min-of-3. Recall@k = each system's index result vs its OWN brute force.

## First run: N=100k, dim=64, k=10, q=20, lists=200, probes=10 (cpu-pinned, one core)

| metric | Lockstep | pgvector |
|---|---|---|
| brute-force k-NN (ms/query) | 26 242 | 55.3 |
| ivfflat build (ms) | 41 584 | 1 860 |
| ivfflat k-NN (ms/query) | 4 267 | 1.77 |
| recall@10 (vs own brute) | 1.000 | 0.090 |

## Honest read — BOTH sides of this table are anomalous; do not quote it as-is

1. **Lockstep does not scale past ~50k rows yet.** Brute k-NN ms/query: 12.5k → 311,
   25k → 774, 50k → 1 553 (≈ linear), **100k → 26 242 (a 17x cliff)**. The cliff shape
   points at a memory blow-up during row materialization (a 64-d row decodes to a
   ~65-Datum vector per row; several concurrent copies per query ≈ GBs → swap), not at
   the distance kernel. Needs a profile-first investigation before any fix.
   At 5k–20k (earlier micro-bench) the engine behaved: ivfflat 0.84 ms/query, 14.7x over
   its own brute, recall 0.958.
2. **The recall comparison is not apples-to-apples on this dataset.** 100 tight clusters
   x 1 000 near-duplicate points make the top-10 sets tie-dominated: Lockstep's fully
   deterministic tie-break (distance, then PK) makes its index and brute paths agree
   exactly (recall 1.0 is a determinism artifact, not superior ANN quality), while
   pgvector's 0.09 reflects tie-shuffling plus k-means degeneracy on duplicate clusters.
   The dataset needs well-separated points (more clusters, larger jitter) before recall
   numbers mean anything.
3. **pgvector's absolute numbers are the target to close**: 1.77 ms/query indexed and a
   1.9 s build at this scale on one core.

## Next steps (tracked in SQL_FEATURES_PLAN)

- Profile the 50k→100k cliff (suspect: per-query full-width row materialization; the ANN
  path also point-gets rows one Query round-trip at a time, ~130 us each).
- Regenerate the dataset with separated points; rerun recall.
- Only then publish a quotable comparison.
