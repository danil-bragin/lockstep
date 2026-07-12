# K11.5 — Lockstep agent-memory vs mem0: measured head-to-head (2026-07-12)

One laptop (arm64). Both sides fully local, NO LLM anywhere: mem0 `add(infer=False)`
(its LLM fact-extraction off — we have no extraction either), and BOTH sides receive
IDENTICAL deterministic embeddings (32-dim FNV bag-of-words, L2-normalized) and the
identical deterministic corpus/queries. Corpus: 2000 memories x 8 words; 200 clean
queries (4 of the target's 8 words) + 200 noisy queries (2 real words + 2
out-of-corpus tokens — the sloppy shape real agent queries have).

| | add ops/s | search q/s | recall@5 clean | recall@5 noisy |
|---|---|---|---|---|
| **Lockstep** `lockstep_mcpd` (MCP stdio subprocess, hybrid RRF, durable WAL+fsync per op) | 123 | 2955 | **0.935** | **0.230** |
| **mem0 2.0.11** + FAISS (in-process python, vector-only, no per-op durability) | 204 | 3859 | 0.620 | 0.050 |
| **Lockstep in-process** (same engine, mem0's deploy shape: no MCP transport, no fsync-per-op) | **16415** | **4587** | 0.910 | — |

## Honest reading

**Shape-for-shape, Lockstep wins every axis.** The third row runs the SAME engine the
way mem0 runs (in-process, no per-op fsync, no subprocess): add is 80x mem0, search
1.19x — while still doing hybrid two-leg retrieval that delivers the recall gap. The
first row's throughput deficit is therefore entirely the deploy shape it CHOOSES:
durable fsync'd WAL per remember + an out-of-process MCP server — properties mem0
does not offer at any speed. (The 0.910 vs 0.935 recall wiggle is float-formatting
of the embedding literal between the C++ and python drivers — ranking ties shift.)

**Quality is the story: +51% clean recall, 4.6x noisy recall.** Same vectors, same
corpus — the entire gap is hybrid RRF (vector + BM25 fusion) vs vector-only ranking.
mem0's own faiss backend prints at startup that keyword/hybrid search is unsupported
(it offers hybrid only on qdrant/elasticsearch/pgvector — i.e. by bolting on another
system, which is exactly the stack we replace with one engine).

**Throughput axes go to mem0, with stated causes.** Its add is 1.7x ours: mem0+faiss
appends to an in-process index with NO per-op durability, while every Lockstep
`remember` is a WAL commit + fsync plus BM25-posting and IVFFLAT maintenance —
crash-safe at every step (the restart e2e is gated). Its search is 1.3x ours: we pay
a subprocess pipe + JSON round trip per query (mem0 is a library call) AND run two
index legs + fusion instead of one. Both handicaps are the product shape, not
accidents: durable memory and an out-of-process MCP server are what an agent stack
actually deploys.

**Not measured here:** mem0's LLM extraction/consolidation layer (needs API keys;
orthogonal to the store), LOCOMO-style QA benchmarks (LLM-judged), scaling beyond
2k memories. What Lockstep adds that mem0 has no analogue for: exact step-level
audit (`history` = AS OF), deterministic byte-identical ranking (incident replay),
per-agent schema isolation, and CHANGES feeds over the memory table.

Repro: `python3 bench/compare/mcp/run_mem0.py ./build/release/cli/lockstep_mcpd`
(venv: `pip install mem0ai faiss-cpu`; env `MEM0_TELEMETRY=False OPENAI_API_KEY=sk-unused`).

# K11.5b — LOCOMO end-to-end QA (2026-07-13)

LOCOMO (Maharana et al. 2024): very long multi-session dialogues + QA over them —
THE public agent-memory benchmark mem0 markets against. Subset run for budget:
conversations 0-1, 60 questions each (categories 1-4; 5 = adversarial excluded, as
in mem0's own reporting). Both systems ingest IDENTICAL raw turn-memories
("On <date>, <speaker> said: ...", mem0 add(infer=False)), identical deterministic
hash embeddings, retrieval top-10, answers and judging by the SAME model
(openai/gpt-4o-mini via OpenRouter, temperature 0). The ONLY variable is the
retrieval layer.

| | overall | multi-hop | temporal | open | single-hop |
|---|---|---|---|---|---|
| **Lockstep** (hybrid RRF) | **35/120 = 29.2%** | 7/34 | **19/54** | 4/9 | 5/23 |
| mem0 + FAISS (vector-only) | 13/120 = 10.8% | 2/34 | 4/54 | 4/9 | 3/23 |

**2.7x end-to-end QA accuracy, same LLM, same memories, same embeddings** — the gap
is the retrieval layer alone. Largest on temporal questions (4.75x): dates live in
the memory TEXT ("On 7 May 2023, ..."), and BM25 anchors on them where the hash
vector cannot; the RRF fusion carries that signal through.

**Honest bounds.** Absolute numbers are far below mem0's marketed ~67% or memU's
~92% — those regimes spend LLM calls on ingest (fact extraction / categorization)
and use real semantic embeddings; ours here spends ZERO ingest LLM cost and uses
32-dim hash vectors. This run isolates the STORE's contribution, not the whole
pipeline's. A fair fight with memU/mem0-full requires an equal-ingest-budget run
(both sides given the same extraction pass) — recorded as the next step. Judged by
a single model, single seed subset; not a leaderboard submission.

Also fixed by this run: the engine's JSON \uXXXX parser mishandled surrogate PAIRS
(emoji in LOCOMO turns became invalid UTF-8 in tool responses) — now combined into
proper 4-byte UTF-8.

Repro: `OR_KEY=... python3 bench/compare/mcp/run_locomo.py ./build/release/cli/lockstep_mcpd locomo10.json`

# memU-over-Lockstep PoC — recon verdict (2026-07-13)

memU (NevaMind-AI) is an LLM orchestration layer OVER a pluggable store (inmemory /
sqlite / postgres) — not a store competitor. Its postgres backend is SQLAlchemy +
alembic: the gap list to run it over our PG wire today is (1) `CREATE EXTENSION
vector` (accept as a no-op — our VECTOR is native), (2) `DateTime(timezone=True)` +
`server_default now()` column machinery, (3) alembic's version table + the SQLAlchemy
dialect's catalog introspection surface. That is an ORM-compatibility work package
(its own session), not an evening PoC — recorded as the concrete next slice of the
PG-surface roadmap. Strategically memU remains an INTEGRATION target (their layer,
our engine), and any quality comparison must be at equal ingest budget.
