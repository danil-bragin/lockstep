#!/usr/bin/env python3
# K11.5b — LOCOMO (long conversational memory QA) head-to-head: Lockstep agent-memory
# (lockstep_mcpd, hybrid RRF recall) vs mem0+FAISS (vector-only), same LLM everywhere.
# Both systems ingest IDENTICAL raw turn-memories (mem0 infer=False — its LLM fact
# extraction is off, stated in the report) with IDENTICAL deterministic embeddings;
# answering and judging use the same model via OpenRouter. Cats 1-4 (5 = adversarial
# "unanswerable" excluded, as in mem0's own reporting).
import json, os, subprocess, sys, time, shutil, urllib.request

MODEL = "openai/gpt-4o-mini"
KEY = os.environ["OR_KEY"]
K = 10                      # retrieved memories per question
CONVS = [0, 1]
QA_PER_CONV = 60

def llm(prompt, max_tokens=80, retries=4):
    body = json.dumps({"model": MODEL, "temperature": 0,
                       "messages": [{"role": "user", "content": prompt}],
                       "max_tokens": max_tokens}).encode()
    for attempt in range(retries):
        try:
            req = urllib.request.Request(
                "https://openrouter.ai/api/v1/chat/completions", data=body,
                headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.load(r)["choices"][0]["message"]["content"].strip()
        except Exception:
            if attempt == retries - 1: raise
            time.sleep(2 * (attempt + 1))

def embed(text):
    v = [0.0] * 32
    for w in text.lower().split():
        h = 2166136261
        for c in w.encode():
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        v[h % 32] += 1.0
    n = sum(x * x for x in v) ** 0.5 or 1.0
    return [x / n for x in v]

# --extract mode (equal-ingest budget): ONE LLM pass per session distills the raw
# turns into fact memories; BOTH systems then ingest the IDENTICAL extracted facts —
# the regime mem0/memU market in, with the extraction cost identical by construction.
def extract_session(date, turns):
    convo = "\n".join(f'{t["speaker"]}: {t.get("text","")}' +
                      (f' [photo: {t["blip_caption"]}]' if t.get("blip_caption") else "")
                      for t in turns)
    out = llm("Extract the important durable facts from this conversation session as a "
              "list, one fact per line, each self-contained and mentioning who and, if "
              "relevant, when. Session date: " + date + "\n\n" + convo +
              "\n\nFacts (one per line):", max_tokens=700)
    return [f'On {date}: {ln.lstrip("-* 0123456789.").strip()}'
            for ln in out.splitlines() if ln.strip()]

def memories_of_extracted(conv):
    out = []
    i = 1
    while f"session_{i}" in conv:
        out.extend(extract_session(conv.get(f"session_{i}_date_time", ""),
                                   conv[f"session_{i}"] or []))
        i += 1
    return out

def memories_of(conv):
    out = []
    i = 1
    while f"session_{i}" in conv:
        date = conv.get(f"session_{i}_date_time", "")
        for t in conv[f"session_{i}"] or []:
            txt = t.get("text", "")
            if t.get("blip_caption"):
                txt += " [shared a photo: " + t["blip_caption"] + "]"
            out.append(f'On {date}, {t["speaker"]} said: {txt}')
        i += 1
    return out

def answer(question, mems):
    ctx = "\n".join("- " + m for m in mems)
    return llm("You answer questions about two people's conversations using ONLY the "
               "memories below. Be concise (a few words; a date if asked when).\n"
               f"Memories:\n{ctx}\n\nQuestion: {question}\nAnswer:")

def judge(question, gold, cand):
    v = llm(f"Question: {question}\nGold answer: {gold}\nCandidate answer: {cand}\n"
            "Does the candidate convey the same essential fact as the gold answer? "
            "Reply with exactly YES or NO.", max_tokens=4)
    return v.upper().startswith("YES")

class Mcp:
    def __init__(self, binary, data_dir):
        shutil.rmtree(data_dir, ignore_errors=True); os.makedirs(data_dir)
        self.p = subprocess.Popen([binary, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.call("initialize", {}, raw=True)
    def call(self, tool, args, raw=False):
        m = {"jsonrpc": "2.0", "id": 1, "method": tool if raw else "tools/call",
             "params": args if raw else {"name": tool, "arguments": args}}
        self.p.stdin.write(json.dumps(m) + "\n"); self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

def run_lockstep(binary, mems, qas):
    s = Mcp(binary, "/tmp/locomo_ls")
    for m in mems:
        r = s.call("remember", {"content": m, "embedding": embed(m)})
        assert not r["result"]["isError"], r
    correct = per = {}
    correct = 0; per = {}
    for q in qas:
        rr = s.call("recall", {"query": q["question"], "embedding": embed(q["question"]), "k": K})
        got = json.loads(rr["result"]["content"][0]["text"])
        mems_k = [g["content"] for g in got]
        cand = answer(q["question"], mems_k)
        ok = judge(q["question"], str(q["answer"]), cand)
        correct += ok
        per.setdefault(q["category"], [0, 0])
        per[q["category"]][0] += ok; per[q["category"]][1] += 1
    s.p.stdin.close()
    return correct, per

def run_mem0(mems, qas):
    from mem0 import Memory
    path = "/tmp/locomo_faiss"; shutil.rmtree(path, ignore_errors=True)
    m = Memory.from_config({
        "embedder": {"provider": "openai", "config": {"api_key": "sk-unused", "embedding_dims": 32}},
        "vector_store": {"provider": "faiss", "config": {"embedding_model_dims": 32, "path": path}},
        "llm": {"provider": "openai", "config": {"api_key": "sk-unused"}}})
    class DetEmb:
        def embed(self, text, memory_action=None): return embed(text)
    m.embedding_model = DetEmb()
    for mm in mems:
        m.add(mm, user_id="u", infer=False)
    correct = 0; per = {}
    for q in qas:
        s = m.search(q["question"], filters={"user_id": "u"}, top_k=K)
        mems_k = [r.get("memory") for r in s.get("results", [])]
        cand = answer(q["question"], mems_k)
        ok = judge(q["question"], str(q["answer"]), cand)
        correct += ok
        per.setdefault(q["category"], [0, 0])
        per[q["category"]][0] += ok; per[q["category"]][1] += 1
    return correct, per

if __name__ == "__main__":
    binary = sys.argv[1]
    data = json.load(open(sys.argv[2]))
    extract = len(sys.argv) > 3 and sys.argv[3] == "--extract"
    total = {"ls": [0, 0], "m0": [0, 0]}
    per_all = {"ls": {}, "m0": {}}
    for ci in CONVS:
        conv = data[ci]
        mems = (memories_of_extracted if extract else memories_of)(conv["conversation"])
        qas = [q for q in conv["qa"] if q.get("category") in (1, 2, 3, 4)][:QA_PER_CONV]
        print(f"conv {ci}: {len(mems)} memories, {len(qas)} questions", flush=True)
        for name, fn in (("ls", lambda: run_lockstep(binary, mems, qas)),
                         ("m0", lambda: run_mem0(mems, qas))):
            c, per = fn()
            total[name][0] += c; total[name][1] += len(qas)
            for k, v in per.items():
                a = per_all[name].setdefault(k, [0, 0])
                a[0] += v[0]; a[1] += v[1]
            print(f"  {name}: {c}/{len(qas)}", flush=True)
    for name, label in (("ls", "lockstep hybrid"), ("m0", "mem0+faiss")):
        c, n = total[name]
        cats = " ".join(f"cat{k}:{v[0]}/{v[1]}" for k, v in sorted(per_all[name].items()))
        print(f"{label}: {c}/{n} = {c/n:.3f}  ({cats})", flush=True)
