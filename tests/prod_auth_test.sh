#!/usr/bin/env bash
# prod_auth_test.sh — APPLICATION-LEVEL AUTH + RBAC over mTLS (Linux-only).
#
# Builds on the mTLS transport (prod_tls_test): mTLS AUTHENTICATES the connection (a cert
# signed by the shared CA), and RBAC AUTHORIZES each operation by mapping the authenticated
# PRINCIPAL (the client cert subject CN) -> a ROLE -> a PERMISSION set over op classes
# (READ = STATUS/METRICS/SELECT; WRITE = SUBMIT; ADMIN = control).
#
# Brings up a single-node Raft cluster (1 lockstepd over mTLS) with an --auth-policy mapping
# four distinct-CN client certs to roles, then asserts the ALLOW/DENY MATRIX + the TEETH:
#   role     SUBMIT(write)  STATUS(read)
#   admin    ALLOW          ALLOW
#   writer   ALLOW          ALLOW
#   reader   DENY           ALLOW
#   unknown  DENY           DENY     (default-deny: not in the policy -> no role -> nothing)
# TEETH:
#   * a reader's SUBMIT is REFUSED and the value did NOT commit (the committed log never
#     contains it) — the permission gate actually blocks the write, not just the reply.
#   * an unknown principal gets NOTHING (status ok=0 AND submit denied).
#   * a writer's SUBMIT still COMMITS (an authorized op succeeds — not a dead node).
#
# PROCESS-CLEANUP (a host freeze happened): every lockstepd is tracked by PID; a trap on
# EXIT/INT/TERM SIGKILLs every tracked child on EVERY path; the daemon self-deadlines
# (--run-seconds) so no orphan can outlive the test. BOUNDED throughout. Run under the
# fork-parent wall-guard (e.g. `to 90 bash tests/prod_auth_test.sh`).

set -u  # NOT -e: run cleanup + print a verdict even on a failed assertion.

# ---- locate the binaries ----------------------------------------------------
LOCKSTEPD="${LOCKSTEPD:-}"
ADMIN="${LOCKSTEP_ADMIN:-}"
find_bin() {
  local name="$1"
  for d in build/ldev build/lrel build cmake-build-debug build/Debug .; do
    if [ -x "$d/cli/$name" ]; then echo "$d/cli/$name"; return 0; fi
    if [ -x "$d/$name" ]; then echo "$d/$name"; return 0; fi
  done
  command -v "$name" 2>/dev/null && return 0
  return 1
}
[ -z "$LOCKSTEPD" ] && LOCKSTEPD="$(find_bin lockstepd)"
[ -z "$ADMIN" ] && ADMIN="$(find_bin lockstep_admin)"
if [ -z "$LOCKSTEPD" ] || [ ! -x "$LOCKSTEPD" ]; then echo "FATAL: lockstepd not found"; exit 2; fi
if [ -z "$ADMIN" ] || [ ! -x "$ADMIN" ]; then echo "FATAL: lockstep_admin not found"; exit 2; fi
command -v openssl >/dev/null 2>&1 || { echo "FATAL: openssl CLI not found"; exit 2; }
echo "lockstepd      = $LOCKSTEPD"
echo "lockstep_admin = $ADMIN"

# ---- topology (single node; ports distinct from the other prod tests) -------
NODE_ID=1
CPORT=19701
APORT=19711
PEERS="--peer 1:${CPORT}"
RUN_SECONDS=60
SEED=20260624

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_auth_XXXXXX")"
CERTS="$WORKDIR/certs"
mkdir -p "$CERTS"
PIDS=()

cleanup() {
  trap - EXIT INT TERM
  for pid in "${PIDS[@]:-}"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null; fi
  done
  wait 2>/dev/null
  rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---- generate dev certs: a CA, a node cert, and per-PRINCIPAL client certs ---
# Each client cert carries a DISTINCT subject CN — that CN is the PRINCIPAL the daemon
# extracts from the verified peer cert and maps to a role via the policy.
echo
echo "=== generate dev certs (CA + node + per-principal client certs) ==="
gen_certs() {
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.crt" \
    -days 1 -subj "/CN=lockstep-dev-ca" >/dev/null 2>&1 || return 1
  # node cert (the daemon's server+client cert for consensus + admin).
  openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/node.key" -out "$CERTS/node.csr" \
    -subj "/CN=lockstep-node" >/dev/null 2>&1 || return 1
  openssl x509 -req -in "$CERTS/node.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
    -CAcreateserial -out "$CERTS/node.crt" -days 1 >/dev/null 2>&1 || return 1
  # per-principal client certs (CN == the principal name). admin/writer/reader are in the
  # policy; unknown-cn is signed by the SAME CA (so the handshake succeeds — it is
  # authenticated) but is NOT mapped to any role (so RBAC default-denies it everything).
  for cn in admin-cn writer-cn reader-cn unknown-cn; do
    openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/$cn.key" -out "$CERTS/$cn.csr" \
      -subj "/CN=$cn" >/dev/null 2>&1 || return 1
    openssl x509 -req -in "$CERTS/$cn.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
      -CAcreateserial -out "$CERTS/$cn.crt" -days 1 >/dev/null 2>&1 || return 1
  done
  return 0
}
if ! gen_certs; then echo "FATAL: cert generation failed"; exit 2; fi
echo "  CA + node + {admin-cn,writer-cn,reader-cn,unknown-cn} client certs generated"

# ---- the RBAC policy file (principal CN -> role) -----------------------------
POLICY="$WORKDIR/auth.policy"
cat >"$POLICY" <<EOF
# principal (cert CN) = role.  unknown-cn is DELIBERATELY absent -> default-DENY.
admin-cn=admin
writer-cn=writer
reader-cn=reader
plaintext=enforce
EOF
echo "  policy: admin-cn=admin writer-cn=writer reader-cn=reader (unknown-cn => default-deny)"

TLS="--tls-cert $CERTS/node.crt --tls-key $CERTS/node.key --tls-ca $CERTS/ca.crt"
client_tls() { echo "--tls-cert $CERTS/$1.crt --tls-key $CERTS/$1.key --tls-ca $CERTS/ca.crt"; }

# ---- launch the daemon with the auth policy ---------------------------------
echo
echo "=== launch 1 lockstepd over mTLS with --auth-policy ==="
mkdir -p "$WORKDIR/node$NODE_ID"
"$LOCKSTEPD" --node-id "$NODE_ID" --listen-port "$CPORT" --admin-port "$APORT" \
  $PEERS --data-dir "$WORKDIR/node$NODE_ID" --seed "$SEED" --run-seconds "$RUN_SECONDS" \
  $TLS --auth-policy "$POLICY" \
  --election-min-ms 30 --election-max-ms 80 --heartbeat-ms 15 \
  >"$WORKDIR/node$NODE_ID.log" 2>&1 &
PIDS[0]=$!
echo "launched node $NODE_ID pid=${PIDS[0]} consensus=$CPORT admin=$APORT (mTLS + RBAC)"

now_ms() { date +%s%3N; }
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }
logfield() { echo "$1" | sed -n "s/.*log=\([^ ]*\).*/\1/p"; }

# status as a given principal; returns the raw STATUS line.
status_as() { "$ADMIN" status $(client_tls "$1") --host "$APORT" 2>/dev/null | head -1; }
# submit as a given principal (accept-only, single host); returns the raw SUBMIT line.
submit_as() { "$ADMIN" submit "$2" --no-await $(client_tls "$1") --host "$APORT" 2>/dev/null | head -1; }

PASS=1
fail() { echo "FAIL: $*"; PASS=0; }

# ---- wait for the node to become LEADER (so an authorized write can commit) --
echo
echo "=== wait for the node to become LEADER ==="
DEADLINE=$(( $(now_ms) + 20000 )); up=0
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  line="$(status_as admin-cn)"
  if [ "$(field "$line" ok)" = "1" ] && [ "$(field "$line" role)" = "2" ]; then up=1; break; fi
  sleep 0.3
done
if [ "$up" = "1" ]; then
  echo "PASS: node is LEADER (reachable over mTLS, admin-cn STATUS ok=1 role=2)"
else
  echo "  log tail:"; tail -8 "$WORKDIR/node$NODE_ID.log"
  fail "node did not become leader / admin-cn could not STATUS"
fi

# ============================================================================
# THE RBAC ALLOW/DENY MATRIX
# ============================================================================
echo
echo "=== RBAC ALLOW/DENY MATRIX ==="
printf "  %-10s %-14s %-14s\n" "principal" "SUBMIT(write)" "STATUS(read)"

# Helpers that turn a raw line into an ALLOW/DENY verdict.
submit_verdict() {  # $1 principal $2 value -> echoes ALLOW or DENY
  local line; line="$(submit_as "$1" "$2")"
  local acc; acc="$(field "$line" accepted)"
  local den; den="$(field "$line" denied)"
  if [ "$acc" = "1" ]; then echo "ALLOW"; elif [ "$den" = "1" ]; then echo "DENY"; else echo "DENY"; fi
}
status_verdict() {  # $1 principal -> echoes ALLOW or DENY
  local line; line="$(status_as "$1")"
  if [ "$(field "$line" ok)" = "1" ]; then echo "ALLOW"; else echo "DENY"; fi
}

A_SUB="$(submit_verdict admin-cn  auth-admin-w)"
A_ST="$(status_verdict admin-cn)"
W_SUB="$(submit_verdict writer-cn auth-writer-w)"
W_ST="$(status_verdict writer-cn)"
R_SUB="$(submit_verdict reader-cn auth-reader-w)"
R_ST="$(status_verdict reader-cn)"
U_SUB="$(submit_verdict unknown-cn auth-unknown-w)"
U_ST="$(status_verdict unknown-cn)"

printf "  %-10s %-14s %-14s\n" "admin"   "$A_SUB" "$A_ST"
printf "  %-10s %-14s %-14s\n" "writer"  "$W_SUB" "$W_ST"
printf "  %-10s %-14s %-14s\n" "reader"  "$R_SUB" "$R_ST"
printf "  %-10s %-14s %-14s\n" "unknown" "$U_SUB" "$U_ST"

# Assert the matrix.
[ "$A_SUB" = "ALLOW" ] || fail "admin SUBMIT should be ALLOW (got $A_SUB)"
[ "$A_ST"  = "ALLOW" ] || fail "admin STATUS should be ALLOW (got $A_ST)"
[ "$W_SUB" = "ALLOW" ] || fail "writer SUBMIT should be ALLOW (got $W_SUB)"
[ "$W_ST"  = "ALLOW" ] || fail "writer STATUS should be ALLOW (got $W_ST)"
[ "$R_SUB" = "DENY"  ] || fail "reader SUBMIT should be DENY (got $R_SUB)"
[ "$R_ST"  = "ALLOW" ] || fail "reader STATUS should be ALLOW (got $R_ST)"
[ "$U_SUB" = "DENY"  ] || fail "unknown SUBMIT should be DENY default-deny (got $U_SUB)"
[ "$U_ST"  = "DENY"  ] || fail "unknown STATUS should be DENY default-deny (got $U_ST)"

# ============================================================================
# TEETH (1): a reader's SUBMIT did NOT commit (the value never appears in the log)
# ============================================================================
echo
echo "=== TEETH(1): reader's REFUSED write did NOT commit ==="
# Attempt the reader write (denied above) + the unknown write (denied above) a few times.
for i in 1 2 3; do
  submit_as reader-cn  "READER-FORBIDDEN-$i"  >/dev/null
  submit_as unknown-cn "UNKNOWN-FORBIDDEN-$i" >/dev/null
done
# Read the committed log AS admin (authorized to STATUS) and confirm NONE of the forbidden
# values are present.
ST_LINE="$(status_as admin-cn)"
LOG="$(logfield "$ST_LINE")"
echo "  committed log (via admin STATUS) = ${LOG:-<empty>}"
if echo ",$LOG," | grep -q "READER-FORBIDDEN"; then
  fail "a reader's REFUSED write COMMITTED (found READER-FORBIDDEN in the log) — gate leaked!"
elif echo ",$LOG," | grep -q "UNKNOWN-FORBIDDEN"; then
  fail "an unknown principal's REFUSED write COMMITTED — default-deny leaked!"
else
  echo "  PASS: no forbidden (reader/unknown) value is in the committed log — the gate BLOCKED the write"
fi

# ============================================================================
# TEETH (2): an AUTHORIZED write still COMMITS (not a dead node)
# ============================================================================
echo
echo "=== TEETH(2): an AUTHORIZED writer's SUBMIT still COMMITS ==="
COMMITTED=0
DEADLINE=$(( $(now_ms) + 10000 ))
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  # durable submit (awaits commit) as the writer.
  out="$("$ADMIN" submit auth-commit-proof $(client_tls writer-cn) --host "$APORT" 2>/dev/null | head -1)"
  if [ "$(field "$out" durable)" = "1" ]; then COMMITTED=1; break; fi
  [ "$(field "$out" accepted)" = "1" ] && { COMMITTED=1; break; }
  sleep 0.3
done
ST2="$(status_as admin-cn)"
LOG2="$(logfield "$ST2")"
if [ "$COMMITTED" = "1" ] && echo ",$LOG2," | grep -q "auth-commit-proof"; then
  echo "  PASS: writer's 'auth-commit-proof' COMMITTED (log=$LOG2) — authorized op succeeds"
else
  fail "writer's authorized SUBMIT did NOT commit (committed=$COMMITTED log=$LOG2) — node dead?"
fi

# ---- verdict ----------------------------------------------------------------
echo
if [ "$PASS" = "1" ]; then
  echo "[prod_auth_test] ALL PASS — RBAC matrix holds + default-deny + denied write did not commit + authorized op succeeds"
  exit 0
else
  echo "[prod_auth_test] FAILED"
  exit 1
fi
