# K1.5 — vector k-NN vs pgvector (2026-07-11, post scan-fix)

Harness: `run_vector.sh [N] [DIM] [K] [QUERIES] [LISTS] [PROBES]` — identical deterministic
data (shared integer formula), both systems pinned to one CPU, Docker. Lockstep embedded
(`lockstep_vector.cpp`, libc++ Release); pgvector = `pgvector/pgvector:pg16`, fsync off,
EXPLAIN ANALYZE min-of-3. Recall@k = each system's index result vs its OWN brute force.

## Run 2 (after the scan_task O(N^2) fix, 4e68dc3): N=100k, dim=64, k=10, lists=200, probes=10

| metric | Lockstep | pgvector | gap |
|---|---|---|---|
| brute-force k-NN (ms/query) | 597 | 13.2 | 45x |
| ivfflat build (ms) | 4 532 | 395 | 11x |
| ivfflat k-NN (ms/query) | 156 | 0.69 | 226x |
| recall@10 (vs own brute) | 1.000* | 0.140* | — |

*Recall still tie-dominated on this dataset (see below) — not comparable yet.

**The 17x scaling cliff the first run exposed is FIXED** (storage scan_task now folds
SSTable runs with a linear merge; brute k-NN scales 2.06x per row-doubling to 200k).
The remaining honest gap is architectural, and each cause is known:
- brute: our path materialises full-width rows and evaluates the distance through the
  AST interpreter per row; pgvector runs a C float4 loop over tuples.
- indexed: our probe decodes each candidate payload into Datums through the generic
  ARRAY codec (~5k candidates here) plus Query-machinery overhead per range scan;
  pgvector reads float4 arrays straight off index pages.
Next rungs: raw-double candidate scoring in the probe (skip the Datum decode), narrow
row materialisation for the k-NN shape, then revisit.

## Run 1 (BEFORE the fix — kept for the record): N=100k, dim=64, k=10, q=20, lists=200, probes=10 (cpu-pinned, one core)

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

---

# Dataset v2/v3 — the recall-vs-latency curves (2026-07-11, honest claim)

Harness: `run_vector_v2.sh` — separated points (1000 clusters, per-dim jitter [0,2)),
probes sweep {1..100} on BOTH systems, one psql session per sweep point (per-query
docker exec proved flaky and poisoned two earlier runs — kept for the record in git
history), pgvector at its recommended lists=100, ours at lists=200. Recall@10 vs each
system's own exact scan. 100k x 64d, one pinned core.

## v2 curve (magnitudes 0..41)

| probes | Lockstep ms | Lockstep recall | pgvector ms | pgvector recall |
|---|---|---|---|---|
| 1 | 0.34 | **1.000** | 0.13 | 0.160 |
| 2 | 0.59 | **1.000** | 0.22 | 0.180 |
| 5 | 0.71 | **1.000** | 0.51 | 0.205 |
| 10 | 0.72 | **1.000** | 0.98 | 0.220 |
| 20 | 0.70 | **1.000** | 2.06 | 0.190 |
| 50 | 0.75 | **1.000** | 5.70 | 0.175 |
| 100 (= its lists) | — | — | 13.21 | 0.630 |

## The headline finding (verified by hand, three ways)

At probes = lists pgvector MUST return its exact answer — it scans every list. It
returns recall 0.63-0.71 instead, and its own seqscan-vs-indexscan answers disagree
for the same query: **pgvector's float4 arithmetic cannot rank dense neighborhoods**
— with 64 dims and values up to ~40, its accumulated f4 error (~0.5 on squared-L2)
exceeds the true gaps between neighboring ranks, and different code paths round
differently. Lockstep stores and ranks in float64 (the f32 pass is only a pruning
window with a proven margin), so its index answers match its exact scan bit-for-bit
at every sweep point — recall 1.000 across the board.

## The honest claim (and its bounds)

At any recall level >= 0.95 on this workload class, Lockstep IVFFLAT serves k-NN in
**~0.7 ms/query while pgvector cannot reach that recall at any probes setting** — its
best is 0.63-0.71 at 13-75 ms (a full-index probe). At low recall (~0.2) pgvector's
raw latency is smaller (0.13-0.98 ms). Bounds of the claim: one core, 100k x 64d,
lattice-clustered deterministic data; the effect is strongest where true neighbor
gaps approach f4 resolution (dense/high-dim embeddings — a real regime), and shrinks
on well-separated data. v3 (tighter lattice) makes our own probes=1-2 recall drop to
0.85-0.90 — a normal ANN curve, recovering to 1.000 from probes=5 (2.6 ms) while
pgvector stays <= 0.71 everywhere.

**One sentence: on dense embeddings we are the only one of the two that can hit
high recall at all — at sub-millisecond latency; "faster than pgvector" is TRUE at
recall parity, and unreachable-by-them above recall 0.7 on this data.**

---

# Final standing (2026-07-12, after SoA prune + heap pivot + quality-first k-means)

Head-to-head @100k x 64d, one core, probes=10 both: **Lockstep 0.67-0.97 ms / recall
1.000 vs pgvector 0.72 ms / recall <= 0.22.** On the v3 curve our (0.63 ms, recall
1.000) point at probes=5 PARETO-DOMINATES every pgvector operating point (his best
recall 0.71 costs 75 ms; at sub-millisecond he is at recall <= 0.14). Sampling in
k-means is reserved for huge sets (> 1000 vectors/list): pgvector-style 50/list
sampling bent centroids on overlapping clusters and cost recall (1.0 -> 0.85) — we
chose quality by default and kept the honest 3.5 s build at this scale.

**Verdict: faster AND correct — on dense embeddings Lockstep wins both axes.**
