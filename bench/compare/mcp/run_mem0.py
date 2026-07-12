#!/usr/bin/env python3
# K11.5 — Lockstep agent-memory (lockstep_mcpd over MCP stdio) vs mem0 (in-process
# python lib, FAISS local store). Both sides get IDENTICAL deterministic embeddings
# (32-dim FNV bag-of-words, L2-normalized) and the IDENTICAL corpus/queries, no LLM
# anywhere (mem0 add(infer=False) = raw memory, its LLM fact-extraction is off; we
# have no extraction either). Honest handicaps, stated: mem0 runs IN-PROCESS while
# Lockstep pays a subprocess pipe + JSON round trip per op; mem0's FAISS leg is
# vector-only (its own docs: no keyword search on faiss) while Lockstep recall is
# hybrid RRF (vector + BM25) — that hybrid IS the product claim.
import json, subprocess, sys, time, os, shutil

N = 2000          # memories
Q = 200           # queries (every 10th memory is a target)
K = 5             # top-k
WORDS = [f"w{i:02d}" for i in range(64)] + [
    "raft", "election", "timeout", "quorum", "snapshot", "compaction", "vector",
    "recall", "bm25", "hybrid", "deploy", "pipeline", "conformance", "gate",
    "agent", "memory", "cursor", "exactly", "once", "replica",
]

def word_at(i, j):  # pseudo-random but fully deterministic spread (no near-duplicates)
    h = (i * 2654435761 + j * 40503 + 0x9E3779B9) & 0xFFFFFFFF
    h ^= h >> 13
    return WORDS[h % len(WORDS)]

def memory_text(i):
    return f"note{i} " + " ".join(word_at(i, j) for j in range(8))

def query_text(i):  # 4 of the memory's 8 words — enough to be distinctive
    return " ".join(word_at(i, j) for j in (0, 2, 4, 6))

def noisy_query_text(i):  # HALF the words replaced by out-of-corpus noise: the
    # vector leg degrades gracefully, BM25 still anchors on the 2 real words —
    # the fusion (hybrid) case agent queries actually look like.
    return f"zzq{i} qqz{i} " + " ".join(word_at(i, j) for j in (0, 4))

def embed(text):
    v = [0.0] * 32
    for w in text.lower().split():
        h = 2166136261
        for c in w.encode():
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        v[h % 32] += 1.0
    n = sum(x * x for x in v) ** 0.5 or 1.0
    return [x / n for x in v]

def bench_mem0():
    from mem0 import Memory
    path = "/tmp/k115_faiss"
    shutil.rmtree(path, ignore_errors=True)
    cfg = {
        "embedder": {"provider": "openai", "config": {"api_key": "sk-unused", "embedding_dims": 32}},
        "vector_store": {"provider": "faiss", "config": {"embedding_model_dims": 32, "path": path}},
        "llm": {"provider": "openai", "config": {"api_key": "sk-unused"}},
    }
    m = Memory.from_config(cfg)
    class DetEmb:
        def embed(self, text, memory_action=None):
            return embed(text)
    m.embedding_model = DetEmb()
    t0 = time.perf_counter()
    for i in range(N):
        m.add(memory_text(i), user_id="bench", infer=False)
    t1 = time.perf_counter()
    hits = noisy = 0
    t2 = time.perf_counter()
    for qi in range(Q):
        i = qi * 10
        s = m.search(query_text(i), filters={"user_id": "bench"}, top_k=K)
        if any(memory_text(i) == r.get("memory") for r in s.get("results", [])):
            hits += 1
    t3 = time.perf_counter()
    for qi in range(Q):
        i = qi * 10
        s = m.search(noisy_query_text(i), filters={"user_id": "bench"}, top_k=K)
        if any(memory_text(i) == r.get("memory") for r in s.get("results", [])):
            noisy += 1
    return {"add_s": t1 - t0, "search_s": t3 - t2, "recall": hits / Q, "noisy": noisy / Q}

class Mcp:
    def __init__(self, binary, data_dir):
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir)
        self.p = subprocess.Popen([binary, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.rpc("initialize", {})
    def rpc(self, method, params):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                                       "params": params}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def call(self, tool, args):
        return self.rpc("tools/call", {"name": tool, "arguments": args})

def bench_lockstep(binary):
    s = Mcp(binary, "/tmp/k115_lockstep")
    t0 = time.perf_counter()
    for i in range(N):
        r = s.call("remember", {"content": memory_text(i), "embedding": embed(memory_text(i))})
        assert not r["result"]["isError"], r
    t1 = time.perf_counter()
    hits = noisy = 0
    t2 = time.perf_counter()
    for qi in range(Q):
        i = qi * 10
        r = s.call("recall", {"query": query_text(i), "embedding": embed(query_text(i)), "k": K})
        if memory_text(i) in r["result"]["content"][0]["text"]:
            hits += 1
    t3 = time.perf_counter()
    for qi in range(Q):
        i = qi * 10
        q = noisy_query_text(i)
        r = s.call("recall", {"query": q, "embedding": embed(q), "k": K})
        if memory_text(i) in r["result"]["content"][0]["text"]:
            noisy += 1
    s.p.stdin.close()
    return {"add_s": t1 - t0, "search_s": t3 - t2, "recall": hits / Q, "noisy": noisy / Q}

if __name__ == "__main__":
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/release/cli/lockstep_mcpd"
    ls = bench_lockstep(binary)
    m0 = bench_mem0()
    for name, r in (("lockstep-mcpd (subprocess+JSON, hybrid RRF)", ls),
                    ("mem0+faiss   (in-process, vector-only)", m0)):
        print(f"{name}: add {N/r['add_s']:8.0f} ops/s | search {Q/r['search_s']:7.0f} q/s "
              f"| recall@{K} {r['recall']:.3f} | noisy recall@{K} {r['noisy']:.3f}")
