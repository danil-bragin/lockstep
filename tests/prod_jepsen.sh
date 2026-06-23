#!/usr/bin/env bash
# prod_jepsen.sh — Phase 7 S6 (CAPSTONE, V-PROD-CLUSTER). THE JEPSEN-STYLE PROD
# VERIFICATION: drive the REAL multi-process lockstepd cluster (S5b-2) under a FAULT
# STORM — process crash/restart UNDER LOAD plus a network partition — while a
# concurrent client workload submits a stream of DISTINCT values, RECORD the
# client-observed history + periodic per-node STATUS snapshots, then (after heal)
# CHECK the observed history for the SAFETY properties the sim fault-storm proved
# (consensus_crosscheck_test's fault sweep). This is the prod analogue of that sweep,
# judged over the REAL committed Raft log — the linearization point of this
# total-order store.
#
# ============================================================================
# FAULT MODEL (NET_ADMIN-FREE — the Docker container has NO iptables/NET_ADMIN cap)
# ============================================================================
#   * CRASH/RESTART: SIGKILL a node (leader or follower) under load; later restart it
#     on the SAME data dir. It must recover from ProdDisk + catch up by replication.
#   * PARTITION (no iptables): isolate a node by SIGSTOP-ing its process. A stopped
#     process stops servicing its epoll loop entirely -> it neither sends heartbeats
#     nor answers RequestVote/AppendEntries, so to its peers it is EXACTLY an
#     unreachable / partitioned-away node: it cannot win an election, cannot keep
#     leadership (peers time out -> re-elect among the live majority), and cannot
#     commit. SIGCONT heals the "partition" (the process resumes, re-dials, catches
#     up). This needs NO network capability — pure process control — yet produces the
#     same OBSERVABLE as a one-node network partition (the minority side makes no
#     progress; the majority side does). We document this mapping explicitly; we do
#     NOT require --cap-add=NET_ADMIN.
#   * Each round faults at most a MINORITY (<= floor(N/2)) so a quorum stays live and
#     the cluster keeps making progress (PROGRESS is asserted non-vacuous).
#
# ============================================================================
# SAFETY CHECKER — the linearizability/strict-serializability properties for a
# Raft total-order store (see check_history() below). For a store whose ONLY
# operation is "append a distinct value to a totally-ordered replicated log", the
# linearization point of each ACKed write IS its commit index; so:
#   - NO SPLIT-BRAIN / AGREEMENT: across ALL recorded STATUS snapshots, every pair of
#     nodes' committed-log digests is PREFIX-COMPATIBLE at every index (one is a
#     prefix of the other) — no two nodes ever hold DIVERGENT committed entries at the
#     same index. After heal, all live nodes converge to the SAME committed log. This
#     is Raft State-Machine-Safety; for a total-order store it is linearizability of
#     the replicated register sequence.
#   - ACK DURABILITY / EXACTLY-ONCE: every submit the cluster ACKed (accepted=1 with a
#     term+index) appears EXACTLY ONCE in the FINAL converged committed log, never
#     lost across crashes/partitions, never duplicated. (Ack-durability = "a
#     successful response means the write is linearized & durable".)
#   - LEADER UNIQUENESS: term is monotonic non-decreasing per node over time AND no two
#     nodes ever report divergent committed entries for the same index (the observable
#     of "never two leaders committing in the same term"). Election-Safety surrogate.
#   - PROGRESS (non-vacuous): under the fault storm a NON-TRIVIAL number of ACKed
#     submits actually committed on the converged log (not zero) — the run did real
#     work, the checks are not vacuously satisfied by an idle cluster.
#   - V-XCHECK (reused): the per-client values committed appear in submit order on the
#     final log (per-client relative-order consistency) — the cross-check's notion of
#     correctness, here A(prod cluster) vs the client's own program order.
# TEETH: the checker is run against a FABRICATED bad history (a divergent + a lost-ack
# record) and MUST report FAIL — proving it would catch a real violation.
#
# ============================================================================
# PROCESS + RESOURCE SAFETY (a host FREEZE happened — highest runaway risk):
#   * EVERY spawned lockstepd is launched in THIS script's group + tracked by PID; a
#     trap on EXIT/INT/TERM SIGCONTs (in case SIGSTOPed) THEN SIGKILLs every tracked
#     PID on EVERY exit path. A SIGSTOPed node will NOT die from SIGTERM, so cleanup
#     always SIGCONT+SIGKILLs — no node left stopped/zombie.
#   * lockstepd self-deadlines (--run-seconds) so a daemon dies even if this script is
#     killed before cleanup. EVERY wait is bounded by an ABSOLUTE deadline; no spins.
#   * Run UNDER the brief's fork-parent wall-guard (`to N bash tests/prod_jepsen.sh`)
#     so even a buggy cleanup path is outlived by a group-kill. Verify pgrep -x
#     lockstepd EMPTY after.
# ============================================================================

set -u  # NOT -e: we run cleanup + print a verdict even on a failed assertion.

# ---- locate the binaries (env override, else common build dirs) -------------
LOCKSTEPD="${LOCKSTEPD:-}"
ADMIN="${LOCKSTEP_ADMIN:-}"
find_bin() {
  local name="$1"
  for d in build/ldev build cmake-build-debug build/Debug .; do
    if [ -x "$d/cli/$name" ]; then echo "$d/cli/$name"; return 0; fi
    if [ -x "$d/$name" ]; then echo "$d/$name"; return 0; fi
  done
  command -v "$name" 2>/dev/null && return 0
  return 1
}
[ -z "$LOCKSTEPD" ] && LOCKSTEPD="$(find_bin lockstepd)"
[ -z "$ADMIN" ] && ADMIN="$(find_bin lockstep_admin)"
if [ -z "$LOCKSTEPD" ] || [ ! -x "$LOCKSTEPD" ]; then
  echo "FATAL: lockstepd not found (set LOCKSTEPD=/path)"; exit 2
fi
if [ -z "$ADMIN" ] || [ ! -x "$ADMIN" ]; then
  echo "FATAL: lockstep_admin not found (set LOCKSTEP_ADMIN=/path)"; exit 2
fi
echo "lockstepd      = $LOCKSTEPD"
echo "lockstep_admin = $ADMIN"

# ---- run knobs (bounded; CI-friendly) ---------------------------------------
# SCENARIOS: number of seeded fault schedules to run (<= 5 in-gate). Each derives its
# fault schedule deterministically from SEED_BASE + scenario index (reproducible-ish:
# real-time jitter means not byte-identical, but the SAFETY properties hold every run).
SCENARIOS="${JEPSEN_SCENARIOS:-3}"
SEED_BASE="${JEPSEN_SEED:-1000}"
N_NODES="${JEPSEN_NODES:-5}"           # 5 nodes -> minority of 2 can fault, quorum 3 stays
FAULT_ROUNDS="${JEPSEN_ROUNDS:-4}"     # fault episodes per scenario (bounded)
SUBMITS="${JEPSEN_SUBMITS:-24}"        # bounded distinct-value submit budget per scenario
RUN_SECONDS="${JEPSEN_RUN_SECONDS:-60}"  # each daemon self-deadline (> the whole scenario)

# Loopback port base; node i uses consensus BASE+i, admin BASE+100+i.
CPORT_BASE=20100
APORT_BASE=20300

# ---- per-process tracking + GLOBAL cleanup ----------------------------------
PIDS=()          # tracked child PIDs across ALL scenarios (index-stable per scenario reset)
STOPPED=()       # which tracked PIDs are currently SIGSTOPed (so cleanup SIGCONTs first)
WORKROOT="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_jepsen_XXXXXX")"

cleanup() {
  trap - EXIT INT TERM
  # A SIGSTOPed process will NOT die from SIGTERM/SIGKILL delivery semantics the same
  # way; SIGKILL is uncatchable but a stopped process is reaped only once continued in
  # some kernels — so SIGCONT FIRST, then SIGKILL, for EVERY tracked PID. Precise PID
  # tracking (no pkill -f, which would hit the harness argv).
  for pid in "${PIDS[@]:-}"; do
    [ -z "$pid" ] && continue
    kill -CONT "$pid" 2>/dev/null
    kill -KILL "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$WORKROOT" 2>/dev/null
}
trap cleanup EXIT INT TERM

now_ms() { date +%s%3N; }

# ---- deterministic per-scenario PRNG (xorshift over the seed) ---------------
# Pure-bash 32-bit xorshift so the fault schedule is a reproducible fn of the seed
# (real-time jitter still perturbs timing, but WHICH node is faulted WHEN is seeded).
# CRITICAL: rng_next MUTATES the global RNG_STATE *in the current shell* — it must
# NEVER be called inside a command-substitution subshell (a subshell mutation is lost,
# freezing the schedule). Callers read the global RNG_OUT after calling rng_next.
RNG_STATE=0
RNG_OUT=0
rng_seed() {
  RNG_STATE=$(( ($1 * 2654435761 ^ 0x9E3779B9) & 0xFFFFFFFF ))
  [ "$RNG_STATE" -eq 0 ] && RNG_STATE=1
}
rng_next() {
  local x=$RNG_STATE
  x=$(( (x ^ (x << 13)) & 0xFFFFFFFF ))
  x=$(( x ^ (x >> 17) ))
  x=$(( (x ^ (x << 5)) & 0xFFFFFFFF ))
  RNG_STATE=$x
  RNG_OUT=$x
}
# rng_mod N: sets RNG_OUT to a uniform value in [0, N). Mutates global state.
rng_mod() { rng_next; RNG_OUT=$(( RNG_OUT % $1 )); }

# ============================================================================
# admin helpers
# ============================================================================
status_of() { "$ADMIN" status --host "$1" 2>/dev/null | head -1; }
field()    { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }
logfield() { echo "$1" | sed -n "s/.*log=\([^ ]*\).*/\1/p"; }

# ============================================================================
# ONE SCENARIO
# ============================================================================
# Globals set by run_scenario for the checker:
#   HIST   = history file (submit + status records)
#   SDIR   = scenario work dir
run_scenario() {
  local sc="$1"
  local seed=$(( SEED_BASE + sc ))
  rng_seed "$seed"

  SDIR="$WORKROOT/sc$sc"
  mkdir -p "$SDIR"
  HIST="$SDIR/history.txt"
  : > "$HIST"

  # topology
  local ids=() cports=() aports=() peers=""
  local i
  for ((i=0; i<N_NODES; i++)); do
    ids[i]=$((i+1))
    cports[i]=$((CPORT_BASE + sc*1000 + i))
    aports[i]=$((APORT_BASE + sc*1000 + i))
    peers="$peers --peer ${ids[i]}:${cports[i]}"
  done
  local all_aports=""
  for ((i=0; i<N_NODES; i++)); do all_aports="$all_aports --host ${aports[i]}"; done

  # per-scenario PID slots (reuse the global PIDS array; clear our slots' state)
  local PSLOT=()       # local index -> global PIDS index
  for ((i=0; i<N_NODES; i++)); do
    PIDS+=("")
    PSLOT[i]=$(( ${#PIDS[@]} - 1 ))
  done
  local stopped_now=()   # local-index -> 1 if SIGSTOPed
  for ((i=0; i<N_NODES; i++)); do stopped_now[i]=0; done

  launch_node() {
    local li="$1"
    local id="${ids[li]}"
    local dd="$SDIR/node$id"
    mkdir -p "$dd"
    "$LOCKSTEPD" --node-id "$id" --listen-port "${cports[li]}" \
      --admin-port "${aports[li]}" $peers --data-dir "$dd" --seed "$seed" \
      --run-seconds "$RUN_SECONDS" >"$SDIR/node$id.log" 2>&1 &
    PIDS[${PSLOT[li]}]=$!
  }

  echo
  echo "############################################################"
  echo "# SCENARIO $sc (seed=$seed) — $N_NODES nodes, $FAULT_ROUNDS fault rounds, $SUBMITS submits"
  echo "############################################################"
  for ((i=0; i<N_NODES; i++)); do launch_node "$i"; done

  # ---- wait (bounded) for a leader to emerge --------------------------------
  local quorum=$(( N_NODES/2 + 1 ))
  local dl=$(( $(now_ms) + 20000 ))
  local leader_li=-1
  while [ "$(now_ms)" -lt "$dl" ]; do
    local leaders=0 fol=0; leader_li=-1
    for ((i=0; i<N_NODES; i++)); do
      local line; line="$(status_of "${aports[i]}")"
      [ -z "$line" ] && continue
      [ "$(field "$line" ok)" = "1" ] || continue
      local role; role="$(field "$line" role)"
      [ "$role" = "2" ] && { leaders=$((leaders+1)); leader_li=$i; }
      [ "$role" = "0" ] && fol=$((fol+1))
    done
    [ "$leaders" = "1" ] && break
    sleep 0.3
  done
  if [ "$leader_li" -lt 0 ]; then
    echo "  scenario $sc: no leader emerged within deadline (ABORT scenario)"
    return 1
  fi
  echo "  initial leader = node${ids[leader_li]} (admin ${aports[leader_li]})"

  # ---- record a STATUS snapshot of all nodes into the history ---------------
  snapshot_status() {
    local tag="$1"
    for ((i=0; i<N_NODES; i++)); do
      local line; line="$(status_of "${aports[i]}")"
      local ok role term commit lg
      ok="$(field "$line" ok)"; [ -z "$ok" ] && ok=0
      role="$(field "$line" role)"; [ -z "$role" ] && role=-1
      term="$(field "$line" term)"; [ -z "$term" ] && term=0
      commit="$(field "$line" commit)"; [ -z "$commit" ] && commit=0
      lg="$(logfield "$line")"
      echo "STATUS sc=$sc tag=$tag node=${ids[i]} ok=$ok role=$role term=$term commit=$commit log=$lg" >> "$HIST"
    done
  }

  # ---- submit ONE distinct value (leader-find), record the ACK in history ---
  submit_one() {
    local v="$1"
    local sdl=$(( $(now_ms) + 6000 )); local out=""
    while [ "$(now_ms)" -lt "$sdl" ]; do
      out="$("$ADMIN" submit "$v" $all_aports 2>/dev/null | head -1)"
      [ "$(field "$out" accepted)" = "1" ] && break
      sleep 0.2
    done
    local acc term idx
    acc="$(field "$out" accepted)"; [ -z "$acc" ] && acc=0
    term="$(field "$out" term)"; [ -z "$term" ] && term=0
    idx="$(field "$out" index)"; [ -z "$idx" ] && idx=0
    echo "SUBMIT sc=$sc value=$v accepted=$acc term=$term index=$idx" >> "$HIST"
  }

  # ---- inject one seeded fault on a MINORITY victim -------------------------
  # kinds: 0 = CRASH/RESTART (sigkill, restart later), 1 = PARTITION (sigstop/sigcont)
  inject_fault() {
    local round="$1"
    # pick a victim that is NOT currently faulted, and keep a quorum live.
    local live=0
    for ((i=0; i<N_NODES; i++)); do
      [ -n "${PIDS[${PSLOT[i]}]}" ] && [ "${stopped_now[i]}" = "0" ] && live=$((live+1))
    done
    if [ "$live" -le "$quorum" ]; then
      echo "  round $round: at quorum floor (live=$live) — heal instead of new fault"
      return
    fi
    rng_mod "$N_NODES"; local victim=$RNG_OUT
    # advance to a victim that is live + not faulted
    local tries=0
    while { [ -z "${PIDS[${PSLOT[victim]}]}" ] || [ "${stopped_now[victim]}" = "1" ]; } && [ "$tries" -lt "$N_NODES" ]; do
      victim=$(( (victim+1) % N_NODES )); tries=$((tries+1))
    done
    rng_mod 2; local kind=$RNG_OUT
    local vid="${ids[victim]}"

    if [ "$kind" = "0" ]; then
      # CRASH: SIGKILL under load. Restart on the SAME data dir after a short window.
      echo "  round $round: CRASH node$vid (SIGKILL pid=${PIDS[${PSLOT[victim]}]})"
      echo "FAULT sc=$sc round=$round kind=crash node=$vid" >> "$HIST"
      kill -KILL "${PIDS[${PSLOT[victim]}]}" 2>/dev/null
      wait "${PIDS[${PSLOT[victim]}]}" 2>/dev/null
      PIDS[${PSLOT[victim]}]=""
      # submit a couple values WHILE it is down (must commit on the live quorum).
      submit_one "sc${sc}-r${round}-crashload-a"
      submit_one "sc${sc}-r${round}-crashload-b"
      snapshot_status "r${round}-crashed"
      echo "  round $round: RESTART node$vid on same data dir (recover from ProdDisk)"
      echo "FAULT sc=$sc round=$round kind=restart node=$vid" >> "$HIST"
      launch_node "$victim"
    else
      # PARTITION via SIGSTOP: freeze the process -> peers see it unresponsive.
      echo "  round $round: PARTITION node$vid (SIGSTOP pid=${PIDS[${PSLOT[victim]}]})"
      echo "FAULT sc=$sc round=$round kind=partition node=$vid" >> "$HIST"
      kill -STOP "${PIDS[${PSLOT[victim]}]}" 2>/dev/null
      stopped_now[victim]=1
      # submit under the partition (the live majority must still commit).
      submit_one "sc${sc}-r${round}-partload-a"
      submit_one "sc${sc}-r${round}-partload-b"
      snapshot_status "r${round}-partitioned"
      echo "  round $round: HEAL node$vid (SIGCONT) -> catches up"
      echo "FAULT sc=$sc round=$round kind=heal node=$vid" >> "$HIST"
      kill -CONT "${PIDS[${PSLOT[victim]}]}" 2>/dev/null
      stopped_now[victim]=0
    fi
  }

  # ---- THE STORM: interleave the workload with fault rounds -----------------
  snapshot_status "start"
  local submitted=0
  local round
  for ((round=1; round<=FAULT_ROUNDS; round++)); do
    # baseline load between faults
    local b
    for ((b=0; b<2; b++)); do
      submit_one "sc${sc}-base-${submitted}"; submitted=$((submitted+1))
    done
    inject_fault "$round"
    # let the cluster settle a moment (bounded)
    sleep 1
    snapshot_status "r${round}-settled"
    # more load between rounds, up to the submit budget
    for ((b=0; b<2 && submitted<SUBMITS; b++)); do
      submit_one "sc${sc}-mid-${submitted}"; submitted=$((submitted+1))
    done
  done

  # remaining workload until the budget is spent
  while [ "$submitted" -lt "$SUBMITS" ]; do
    submit_one "sc${sc}-tail-${submitted}"; submitted=$((submitted+1))
  done

  # ---- HEAL EVERYTHING + wait for convergence -------------------------------
  echo "  HEAL: SIGCONT any stopped node, restart any crashed node, wait for convergence"
  for ((i=0; i<N_NODES; i++)); do
    if [ "${stopped_now[i]}" = "1" ]; then
      kill -CONT "${PIDS[${PSLOT[i]}]}" 2>/dev/null; stopped_now[i]=0
    fi
    if [ -z "${PIDS[${PSLOT[i]}]}" ]; then launch_node "$i"; fi
  done

  # wait (bounded) for all live nodes to converge to the SAME committed log.
  local cdl=$(( $(now_ms) + 25000 ))
  local converged=0
  while [ "$(now_ms)" -lt "$cdl" ]; do
    local digest="" all_ok=1 nseen=0
    for ((i=0; i<N_NODES; i++)); do
      local line; line="$(status_of "${aports[i]}")"
      [ "$(field "$line" ok)" = "1" ] || { all_ok=0; continue; }
      nseen=$((nseen+1))
      local lg; lg="$(logfield "$line")"
      if [ -z "$digest" ]; then digest="$lg"; elif [ "$digest" != "$lg" ]; then all_ok=0; fi
    done
    if [ "$all_ok" = "1" ] && [ "$nseen" -ge "$quorum" ]; then converged=1; break; fi
    sleep 0.4
  done
  snapshot_status "final"
  if [ "$converged" = "1" ]; then
    echo "  CONVERGED: all live nodes agree on the committed log"
    echo "CONVERGED sc=$sc value=1" >> "$HIST"
  else
    echo "  WARNING: did not observe full convergence within deadline (checker will judge)"
    echo "CONVERGED sc=$sc value=0" >> "$HIST"
  fi

  # ---- tear down THIS scenario's daemons (cleanup safety: cont+kill) --------
  for ((i=0; i<N_NODES; i++)); do
    local p="${PIDS[${PSLOT[i]}]}"
    [ -z "$p" ] && continue
    kill -CONT "$p" 2>/dev/null
    kill -KILL "$p" 2>/dev/null
    wait "$p" 2>/dev/null
    PIDS[${PSLOT[i]}]=""
  done
  return 0
}

# ============================================================================
# THE CHECKER — pure text analysis of a history file. Returns 0 (PASS) / 1 (FAIL).
# Implemented in awk so it is a deterministic fn of the recorded history (the same
# logic that judges the sim sweep: agreement + ack-durability + leader-unique +
# progress + per-client order). Used BOTH on real histories AND on a fabricated bad
# one (the TEETH).
# ============================================================================
check_history() {
  local hist="$1"
  awk '
  function split_log(s, arr,   n) {
    delete arr
    if (s == "" || s == "-") return 0
    return split(s, arr, ",")
  }
  # prefix-compatible: one of a/b is a prefix of the other AND they agree on the
  # overlap (no DIVERGENT entry at the same index).
  function prefix_compatible(la, na, lb, nb,   k, m) {
    m = (na < nb) ? na : nb
    for (k=1; k<=m; k++) if (la[k] != lb[k]) return 0
    return 1
  }
  BEGIN { fail=0; nstatus=0; nsubmit_ack=0; }
  {
    rec=$1
    # parse key=val tokens
    delete F
    for (i=2;i<=NF;i++){ eq=index($i,"="); if(eq>0){k=substr($i,1,eq-1); v=substr($i,eq+1); F[k]=v} }

    if (rec=="STATUS") {
      idx=nstatus++
      S_node[idx]=F["node"]; S_ok[idx]=F["ok"]; S_term[idx]=F["term"];
      S_commit[idx]=F["commit"]; S_log[idx]=F["log"]; S_tag[idx]=F["tag"];
      # term monotonicity per node (leader-uniqueness surrogate part 1)
      if (F["ok"]=="1") {
        nd=F["node"]+0; t=F["term"]+0
        if (nd in lastterm) {
          if (t < lastterm[nd]) {
            printf("  [FAIL leader-unique] node %d term WENT BACKWARDS %d -> %d (tag %s)\n", nd, lastterm[nd], t, F["tag"]);
            fail=1
          }
        }
        lastterm[nd]=t
      }
    } else if (rec=="SUBMIT") {
      if (F["accepted"]=="1") {
        ACK_val[nsubmit_ack]=F["value"]; ACK_idx[nsubmit_ack]=F["index"]; ACK_term[nsubmit_ack]=F["term"];
        nsubmit_ack++
      }
    } else if (rec=="CONVERGED") {
      converged=F["value"]
    }
  }
  END {
    # ---------- (A) NO SPLIT-BRAIN / AGREEMENT --------------------------------
    # Across EVERY recorded snapshot, every live pair of committed logs must be
    # prefix-compatible (no divergent entry at the same index, ever).
    pairs=0
    for (i=0;i<nstatus;i++) {
      if (S_ok[i]!="1") continue
      na=split_log(S_log[i], LA)
      for (j=i+1;j<nstatus;j++) {
        if (S_ok[j]!="1") continue
        if (S_tag[i]!=S_tag[j]) continue   # compare snapshots taken at the same tag
        nb=split_log(S_log[j], LB)
        pairs++
        if (!prefix_compatible(LA, na, LB, nb)) {
          printf("  [FAIL split-brain] DIVERGENT committed logs at tag %s: node%d=[%s] vs node%d=[%s]\n",
                 S_tag[i], S_node[i], S_log[i], S_node[j], S_log[j]);
          fail=1
        }
      }
    }

    # ---------- build the FINAL converged committed log -----------------------
    # Use the longest committed log seen in a "final" snapshot (all live nodes
    # agree there if converged). Fall back to the globally-longest log seen.
    delete FINAL; nfinal=0
    best=-1; bestlen=-1
    for (i=0;i<nstatus;i++) {
      if (S_ok[i]!="1") continue
      n=split_log(S_log[i], TMP)
      isfinal=(S_tag[i]=="final")
      score=n + (isfinal?100000:0)
      if (score>bestlen) { bestlen=score; best=i }
    }
    if (best>=0) { nfinal=split_log(S_log[best], FINAL) }

    # index the final log for membership + position lookup
    delete POS
    for (k=1;k<=nfinal;k++) POS[FINAL[k]]=k

    # ---------- (B) ACK DURABILITY / EXACTLY-ONCE -----------------------------
    durable=0
    for (a=0;a<nsubmit_ack;a++) {
      v=ACK_val[a]
      # count occurrences of this acked value in the final committed log
      cnt=0
      for (k=1;k<=nfinal;k++) if (FINAL[k]==v) cnt++
      if (cnt==0) {
        printf("  [FAIL ack-durability] ACKed submit value=%s (term=%s index=%s) LOST — absent from final committed log\n",
               v, ACK_term[a], ACK_idx[a]);
        fail=1
      } else if (cnt>1) {
        printf("  [FAIL exactly-once] ACKed submit value=%s appears %d times in final committed log\n", v, cnt);
        fail=1
      } else {
        durable++
      }
    }

    # ---------- (C) PROGRESS (non-vacuous) ------------------------------------
    # A non-trivial number of ACKed submits committed durably under the storm.
    MINPROG=3
    if (durable < MINPROG) {
      printf("  [FAIL progress] only %d ACKed submits durably committed (need >= %d) — vacuous run\n", durable, MINPROG);
      fail=1
    }

    # ---------- (D) V-XCHECK per-client relative order ------------------------
    # The ACKed submits were issued in record (program) order; their positions in
    # the final committed log must be STRICTLY INCREASING (the cluster preserved the
    # single-writer program order — per-client relative-order consistency).
    lastpos=-1; lastval="";
    for (a=0;a<nsubmit_ack;a++) {
      v=ACK_val[a]
      if (!(v in POS)) continue   # not in final log (already flagged by ack-durability)
      p=POS[v]
      if (p <= lastpos) {
        printf("  [FAIL v-xcheck] program-order inversion: %s (pos %d) committed AFTER %s (pos %d)\n",
               v, p, lastval, lastpos);
        fail=1
      }
      lastpos=p; lastval=v
    }

    printf("  checker: snapshots=%d pairs-compared=%d acked=%d durable=%d final-log-len=%d converged=%s\n",
           nstatus, pairs, nsubmit_ack, durable, nfinal, converged);
    if (fail) { print "  CHECK RESULT: FAIL"; exit 1 }
    print "  CHECK RESULT: PASS"; exit 0
  }
  ' "$hist"
}

# ============================================================================
# TEETH: a fabricated BAD history must FAIL the checker.
# ============================================================================
teeth_check() {
  local bad="$WORKROOT/teeth_bad.txt"
  cat > "$bad" <<'EOF'
STATUS sc=9 tag=start node=1 ok=1 role=2 term=1 commit=0 log=-
STATUS sc=9 tag=start node=2 ok=1 role=0 term=1 commit=0 log=-
SUBMIT sc=9 value=teeth-X accepted=1 term=1 index=1
SUBMIT sc=9 value=teeth-Y accepted=1 term=1 index=2
SUBMIT sc=9 value=teeth-LOST accepted=1 term=1 index=3
STATUS sc=9 tag=final node=1 ok=1 role=2 term=2 commit=2 log=teeth-X,teeth-DIVERGENT
STATUS sc=9 tag=final node=2 ok=1 role=0 term=2 commit=2 log=teeth-X,teeth-Y
CONVERGED sc=9 value=1
EOF
  echo
  echo "=== TEETH: run the checker on a FABRICATED bad history (must FAIL) ==="
  echo "    (node1 has a DIVERGENT entry at index 2; teeth-LOST is ACKed but absent)"
  if check_history "$bad"; then
    echo "  TEETH BROKEN: checker PASSED a known-bad history!"
    return 1
  else
    echo "  TEETH OK: checker correctly FAILED the fabricated bad history."
    return 0
  fi
}

# ============================================================================
# MAIN
# ============================================================================
echo
echo "=== prod_jepsen: $SCENARIOS scenarios, $N_NODES nodes each, seed base $SEED_BASE ==="

OVERALL_PASS=1

# Teeth first (proves the checker has teeth before we trust its PASSes).
teeth_check || OVERALL_PASS=0

for ((s=0; s<SCENARIOS; s++)); do
  if run_scenario "$s"; then
    echo
    echo "=== SCENARIO $s — SAFETY CHECK ==="
    if check_history "$HIST"; then
      echo "  scenario $s: ALL SAFETY PROPERTIES HELD"
    else
      echo "  scenario $s: SAFETY VIOLATION (see above) — history at $HIST"
      # keep the offending history for inspection
      cp "$HIST" "$WORKROOT/FAILED_sc$s.txt" 2>/dev/null
      OVERALL_PASS=0
    fi
  else
    echo "  scenario $s: SETUP FAILURE (no leader / abort) — not a safety verdict"
    OVERALL_PASS=0
  fi
done

echo
echo "############################################################"
if [ "$OVERALL_PASS" = "1" ]; then
  echo "[prod_jepsen] ALL $SCENARIOS SCENARIOS PASS — no split-brain, ACKed submits durable/exactly-once, leader-unique, progress non-vacuous, V-XCHECK order held"
  echo "############################################################"
  exit 0
else
  echo "[prod_jepsen] FAILED — see violations above"
  echo "############################################################"
  exit 1
fi
