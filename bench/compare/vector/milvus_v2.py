# milvus_v2.py — Milvus (Lite, in-process segcore: its BEST latency case, no gRPC) on the
# SAME deterministic v3 dataset + probes sweep as run_vector_v2.sh. Emits the same jsonl.
#   python3 milvus_v2.py N DIM K Q NLIST
import sys, time
import numpy as np
from pymilvus import MilvusClient, DataType

N, DIM, K, Q, NLIST = (int(x) for x in sys.argv[1:6])

i = np.arange(N, dtype=np.int64)[:, None]
d = np.arange(DIM, dtype=np.int64)[None, :]
X = ((((i % 1000) * 37 + d * 11) % 8) + ((i * 2654435761 + d * 40503) % 2000) / 1000.0).astype(np.float32)
j = np.arange(Q, dtype=np.int64)[:, None]
QV = ((((((j * 37) % 1000) * 37 + d * 11) % 8) + ((j * 13 + d * 7) % 100) / 100.0)).astype(np.float32)

client = MilvusClient("/tmp/milvus_bench.db")
if client.has_collection("docs"):
    client.drop_collection("docs")
schema = client.create_schema(auto_id=False)
schema.add_field("id", DataType.INT64, is_primary=True)
schema.add_field("v", DataType.FLOAT_VECTOR, dim=DIM)
client.create_collection("docs", schema=schema)
t0 = time.time()
B = 2000
for s in range(0, N, B):
    rows = [{"id": int(s + r), "v": X[s + r].tolist()} for r in range(min(B, N - s))]
    client.insert("docs", rows)
print(f'# loaded {N} in {time.time()-t0:.1f}s', file=sys.stderr)

def build(index_type, params):
    try:
        client.release_collection("docs")
    except Exception:
        pass
    try:
        client.drop_index("docs", "v")
    except Exception:
        pass
    ip = client.prepare_index_params()
    ip.add_index(field_name="v", index_type=index_type, metric_type="L2", params=params)
    t = time.time()
    client.create_index("docs", ip)
    client.load_collection("docs")
    return (time.time() - t) * 1000

def sweep(tag, probes, params):
    # warm
    for q in QV[: min(4, Q)]:
        client.search("docs", data=[q.tolist()], limit=K, anns_field="v", search_params=params)
    ids, t = [], time.time()
    for q in QV:
        res = client.search("docs", data=[q.tolist()], limit=K, anns_field="v", search_params=params)
        ids.append({int(h["id"]) for h in res[0]})
    ms = (time.time() - t) * 1000 / Q
    return ids, ms

bt = build("FLAT", {})
ref, brute_ms = sweep("flat", 0, {"metric_type": "L2"})
print(f'{{"sys":"milvus","probes":0,"ms_each":{brute_ms:.2f},"recall":1.0}}')
bt = build("IVF_FLAT", {"nlist": NLIST})
print(f'{{"sys":"milvus","probes":-1,"ms_each":{bt:.0f},"recall":0}}  # ivf build ms', file=sys.stderr)
for p in (1, 2, 5, 10, 20, 50, NLIST):
    ann, ms = sweep("ivf", p, {"metric_type": "L2", "params": {"nprobe": p}})
    hit = sum(len(ref[q] & ann[q]) for q in range(Q))
    tot = sum(len(ref[q]) for q in range(Q))
    print(f'{{"sys":"milvus","probes":{p},"ms_each":{ms:.2f},"recall":{hit/tot:.4f}}}')
