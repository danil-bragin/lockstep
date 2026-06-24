#!/usr/bin/env bash
# prod_tls_test.sh — TLS TRANSPORT ENCRYPTION (Linux-only). Orchestrate a REAL 3-process
# Raft cluster whose ENTIRE transport (consensus peer replication + admin/client) is wrapped
# in mTLS, and assert:
#   (1) ENCRYPTED CLUSTER WORKS: 3 nodes elect ONE leader + replicate/commit a stream of
#       values over the mTLS wire (a real TLS handshake happened on every link), and a
#       client SUBMITs + reads back over TLS — the verified stack runs over an encrypted wire.
#   (2) TEETH — the encryption + auth actually GATE (not cosmetic):
#       (2a) a PLAINTEXT client (no TLS) is REFUSED by the TLS daemon (no committed reply).
#       (2b) a WRONG-CA client (cert signed by a different CA) is REFUSED (handshake fails).
#       A control re-run with the CORRECT cert SUCCEEDS, proving the refusal is the auth gate,
#       not just an unreachable node.
#
# WHY THIS PROVES ENCRYPTION: a plaintext byte stream is NOT accepted (the daemon speaks
# only TLS now), and a peer/client whose cert is not signed by the shared dev CA fails the
# TLS handshake (SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT — mTLS). So bytes on the wire are a
# real TLS record stream, mutually authenticated against the CA.
#
# DEV CERTS: generated in a scratch dir with the `openssl` CLI — a self-signed CA, then a
# node cert + a client cert signed by it, plus a SEPARATE rogue CA + rogue client cert for
# the wrong-CA teeth. All ephemeral (rm -rf on exit).
#
# PROCESS-CLEANUP GUARANTEE (a host freeze happened): every lockstepd is tracked by PID; a
# trap on EXIT/INT/TERM SIGKILLs every tracked child on EVERY path; each lockstepd self-
# deadlines (--run-seconds) so no orphan can outlive the test. BOUNDED throughout. Run under
# the fork-parent wall-guard (e.g. `to 90 bash tests/prod_tls_test.sh`).

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

# ---- topology (fixed loopback ports; distinct from the plaintext smoke test) -
IDS=(1 2 3)
CPORTS=(19501 19502 19503)   # consensus ports
APORTS=(19601 19602 19603)   # admin ports
PEERS="--peer 1:${CPORTS[0]} --peer 2:${CPORTS[1]} --peer 3:${CPORTS[2]}"
RUN_SECONDS=60
SEED=20260624

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_tls_XXXXXX")"
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

# ---- generate dev certs (CA + node + client; plus a ROGUE CA + rogue client) -
echo
echo "=== generate dev mTLS certs (self-signed CA, node + client certs) ==="
gen_certs() {
  # The shared dev CA.
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.crt" \
    -days 1 -subj "/CN=lockstep-dev-ca" >/dev/null 2>&1 || return 1
  # A node/server+client cert signed by the CA (used by every daemon for BOTH consensus and
  # admin, and by the client). SAN includes localhost/127.0.0.1 though we do not host-verify
  # (cert-chain + CA auth is the gate here, not hostname).
  for who in node client; do
    openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/$who.key" -out "$CERTS/$who.csr" \
      -subj "/CN=lockstep-$who" >/dev/null 2>&1 || return 1
    openssl x509 -req -in "$CERTS/$who.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
      -CAcreateserial -out "$CERTS/$who.crt" -days 1 >/dev/null 2>&1 || return 1
  done
  # A ROGUE CA + a rogue client cert signed by IT (the wrong-CA teeth).
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/rogue-ca.key" \
    -out "$CERTS/rogue-ca.crt" -days 1 -subj "/CN=rogue-ca" >/dev/null 2>&1 || return 1
  openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/rogue.key" -out "$CERTS/rogue.csr" \
    -subj "/CN=rogue-client" >/dev/null 2>&1 || return 1
  openssl x509 -req -in "$CERTS/rogue.csr" -CA "$CERTS/rogue-ca.crt" \
    -CAkey "$CERTS/rogue-ca.key" -CAcreateserial -out "$CERTS/rogue.crt" -days 1 \
    >/dev/null 2>&1 || return 1
  return 0
}
if ! gen_certs; then echo "FATAL: cert generation failed"; exit 2; fi
echo "  CA + node + client + rogue-CA + rogue-client certs generated in $CERTS"

TLS="--tls-cert $CERTS/node.crt --tls-key $CERTS/node.key --tls-ca $CERTS/ca.crt"
CLI_TLS="--tls-cert $CERTS/client.crt --tls-key $CERTS/client.key --tls-ca $CERTS/ca.crt"
ROGUE_TLS="--tls-cert $CERTS/rogue.crt --tls-key $CERTS/rogue.key --tls-ca $CERTS/rogue-ca.crt"

launch() {
  local i="$1"; local id="${IDS[$i]}"; local dd="$WORKDIR/node$id"
  mkdir -p "$dd"
  "$LOCKSTEPD" --node-id "$id" --listen-port "${CPORTS[$i]}" --admin-port "${APORTS[$i]}" \
    $PEERS --data-dir "$dd" --seed "$SEED" --run-seconds "$RUN_SECONDS" $TLS \
    >"$WORKDIR/node$id.log" 2>&1 &
  local pid=$!; PIDS[$i]=$pid
  echo "launched node $id pid=$pid consensus=${CPORTS[$i]} admin=${APORTS[$i]} (mTLS)"
}

now_ms() { date +%s%3N; }
status_of() { "$ADMIN" status $CLI_TLS --host "$1" 2>/dev/null | head -1; }
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }
logfield() { echo "$1" | sed -n "s/.*log=\([^ ]*\).*/\1/p"; }

PASS=1
fail() { echo "FAIL: $*"; PASS=0; }

echo
echo "=== launch 3 lockstepd processes over mTLS ==="
for i in 0 1 2; do launch "$i"; done

# ---- (1) ELECTION over mTLS -------------------------------------------------
echo
echo "=== (1) ELECTION over mTLS — wait for one LEADER, two FOLLOWERS ==="
DEADLINE=$(( $(now_ms) + 25000 ))
LEADER_PORT=""; LEADER_ID=""
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  leaders=0; followers=0; LEADER_PORT=""; LEADER_ID=""
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"
    [ -z "$line" ] && continue
    [ "$(field "$line" ok)" = "1" ] || continue
    role="$(field "$line" role)"
    if [ "$role" = "2" ]; then leaders=$((leaders+1)); LEADER_PORT="${APORTS[$i]}"; LEADER_ID="${IDS[$i]}"; fi
    if [ "$role" = "0" ]; then followers=$((followers+1)); fi
  done
  if [ "$leaders" = "1" ] && [ "$followers" = "2" ]; then break; fi
  sleep 0.3
done
if [ "$leaders" = "1" ] && [ "$followers" = "2" ]; then
  echo "PASS election (mTLS): leader=node$LEADER_ID, 2 followers"
else
  echo "  logs:"; for i in 0 1 2; do echo "  -- node${IDS[$i]} --"; tail -5 "$WORKDIR/node${IDS[$i]}.log"; done
  fail "no unique leader over mTLS within deadline (leaders=$leaders followers=$followers)"
fi

# ---- (2) REPLICATED COMMIT over mTLS + client read over TLS ------------------
echo
echo "=== (2) REPLICATED COMMIT over mTLS — submit values, all 3 agree ==="
VALUES=(tls-alpha tls-bravo tls-charlie tls-delta)
ALL_APORTS="--host ${APORTS[0]} --host ${APORTS[1]} --host ${APORTS[2]}"
for v in "${VALUES[@]}"; do
  sdl=$(( $(now_ms) + 10000 )); ok=0
  while [ "$(now_ms)" -lt "$sdl" ]; do
    out="$("$ADMIN" submit "$v" $CLI_TLS $ALL_APORTS 2>/dev/null | head -1)"
    if [ "$(field "$out" accepted)" = "1" ]; then
      echo "  submitted $v over TLS -> $(echo "$out" | sed 's/^SUBMIT //')"; ok=1; break
    fi
    sleep 0.3
  done
  [ "$ok" = "1" ] || fail "TLS submit $v not accepted"
done

NVALS=${#VALUES[@]}
EXPECTED="$(IFS=,; echo "${VALUES[*]}")"
DEADLINE=$(( $(now_ms) + 25000 )); agreed=0
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  ok_all=1; digest=""; mismatch=0
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"
    if [ "$(field "$line" ok)" != "1" ]; then ok_all=0; break; fi
    commit="$(field "$line" commit)"; lg="$(logfield "$line")"
    if [ -z "$commit" ] || [ "$commit" -lt "$NVALS" ]; then ok_all=0; break; fi
    prefix="$(echo "$lg" | cut -d, -f1-"$NVALS")"
    if [ -z "$digest" ]; then digest="$prefix"; elif [ "$digest" != "$prefix" ]; then mismatch=1; fi
  done
  if [ "$ok_all" = "1" ] && [ "$mismatch" = "0" ] && [ "$digest" = "$EXPECTED" ]; then agreed=1; break; fi
  sleep 0.3
done
if [ "$agreed" = "1" ]; then
  echo "PASS replication over mTLS: all 3 nodes commit>=$NVALS and committed logs AGREE ($EXPECTED)"
  echo "  client READ-BACK over TLS confirms committed log == $EXPECTED"
else
  echo "  last seen:"; for i in 0 1 2; do echo "    node${IDS[$i]}: $(status_of "${APORTS[$i]}")"; done
  fail "nodes did not agree over mTLS within deadline"
fi

# ---- (3) TEETH — plaintext + wrong-CA clients are REFUSED -------------------
echo
echo "=== (3) TEETH — a PLAINTEXT client and a WRONG-CA client are REFUSED ==="

# Control: the CORRECT-cert client succeeds against the leader (proves the node is up).
CTRL="$("$ADMIN" submit tls-control $CLI_TLS --host "$LEADER_PORT" 2>/dev/null | head -1)"
if [ "$(field "$CTRL" accepted)" = "1" ]; then
  echo "  control: correct-cert client SUBMIT accepted=1 (node is up + reachable over TLS)"
else
  fail "control correct-cert client could not submit (test setup issue)"
fi

# (3a) PLAINTEXT client: NO --tls-* flags -> the client speaks cleartext to a TLS daemon.
# The daemon expects a TLS ClientHello; the cleartext bytes do not parse as TLS, so the
# handshake never completes -> no reply -> accepted=0 / ok=0. A plaintext stream is NOT
# accepted.
echo "  (3a) plaintext client (no TLS) -> expect REFUSED..."
PT_STATUS="$("$ADMIN" status --host "$LEADER_PORT" 2>/dev/null | head -1)"
PT_SUBMIT="$("$ADMIN" submit pt-evil --host "$LEADER_PORT" 2>/dev/null | head -1)"
PT_OK="$(field "$PT_STATUS" ok)"; [ -z "$PT_OK" ] && PT_OK=0
PT_ACC="$(field "$PT_SUBMIT" accepted)"; [ -z "$PT_ACC" ] && PT_ACC=0
if [ "$PT_OK" != "1" ] && [ "$PT_ACC" != "1" ]; then
  echo "       PASS: plaintext client REFUSED (status ok=$PT_OK, submit accepted=$PT_ACC)"
else
  fail "plaintext client was NOT refused (status ok=$PT_OK submit accepted=$PT_ACC) — wire not encrypted!"
fi

# (3b) WRONG-CA client: a cert signed by a DIFFERENT CA. The mTLS handshake fails server-side
# (the daemon's CA does not vouch for the rogue cert) -> no reply -> accepted=0 / ok=0.
echo "  (3b) wrong-CA client (rogue cert) -> expect REFUSED..."
WC_STATUS="$("$ADMIN" status $ROGUE_TLS --host "$LEADER_PORT" 2>/dev/null | head -1)"
WC_SUBMIT="$("$ADMIN" submit wc-evil $ROGUE_TLS --host "$LEADER_PORT" 2>/dev/null | head -1)"
WC_OK="$(field "$WC_STATUS" ok)"; [ -z "$WC_OK" ] && WC_OK=0
WC_ACC="$(field "$WC_SUBMIT" accepted)"; [ -z "$WC_ACC" ] && WC_ACC=0
if [ "$WC_OK" != "1" ] && [ "$WC_ACC" != "1" ]; then
  echo "       PASS: wrong-CA client REFUSED (status ok=$WC_OK, submit accepted=$WC_ACC)"
else
  fail "wrong-CA client was NOT refused (status ok=$WC_OK submit accepted=$WC_ACC) — auth not gating!"
fi

# Re-control AFTER the teeth: the correct-cert client STILL works (the refusals above were
# the auth gate, not the node having died).
CTRL2="$("$ADMIN" submit tls-control2 $CLI_TLS --host "$LEADER_PORT" 2>/dev/null | head -1)"
if [ "$(field "$CTRL2" accepted)" = "1" ]; then
  echo "  re-control: correct-cert client STILL accepted=1 (refusals were the auth gate)"
else
  fail "correct-cert client stopped working after teeth (node down?)"
fi

# ---- (4) KILL + RESTART over mTLS (reconnect over the encrypted transport) --
echo
echo "=== (4) KILL a follower, RESTART it over mTLS, wait for CATCH-UP ==="
VICTIM=-1
for i in 0 1 2; do
  if [ "${IDS[$i]}" != "$LEADER_ID" ]; then VICTIM=$i; break; fi
done
if [ "$VICTIM" -lt 0 ]; then
  fail "could not pick a follower to kill"
else
  vid="${IDS[$VICTIM]}"; vpid="${PIDS[$VICTIM]}"
  echo "  killing follower node$vid pid=$vpid (SIGKILL)"
  kill -KILL "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null; PIDS[$VICTIM]=""
  EXTRA=tls-echo
  sdl=$(( $(now_ms) + 10000 )); ok=0
  while [ "$(now_ms)" -lt "$sdl" ]; do
    out="$("$ADMIN" submit "$EXTRA" $CLI_TLS $ALL_APORTS 2>/dev/null | head -1)"
    [ "$(field "$out" accepted)" = "1" ] && { echo "  submitted $EXTRA over mTLS while node$vid down"; ok=1; break; }
    sleep 0.3
  done
  [ "$ok" = "1" ] || fail "TLS submit $EXTRA (victim down) not accepted"
  # The committed log now also contains the teeth-phase control values; rather than rebuild
  # the exact sequence, the catch-up target is the LEADER's actual committed log/commit — the
  # restarted node must re-handshake over mTLS and converge to it (with tls-echo present).
  LEAD_LINE="$(status_of "$LEADER_PORT")"
  TARGET_COMMIT="$(field "$LEAD_LINE" commit)"
  TARGET_LOG="$(logfield "$LEAD_LINE")"
  echo "  restarting node$vid over mTLS on the SAME data dir -> must re-handshake + catch up to commit=$TARGET_COMMIT"
  launch "$VICTIM"
  DEADLINE=$(( $(now_ms) + 25000 )); caught=0
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    line="$(status_of "${APORTS[$VICTIM]}")"
    if [ "$(field "$line" ok)" = "1" ]; then
      commit="$(field "$line" commit)"
      lg="$(logfield "$line")"
      if [ -n "$commit" ] && [ "$commit" -ge "$TARGET_COMMIT" ] && [ "$lg" = "$TARGET_LOG" ]; then
        caught=1; break
      fi
    fi
    sleep 0.3
  done
  if [ "$caught" = "1" ]; then
    case ",$TARGET_LOG," in
      *,"$EXTRA",*) echo "PASS catch-up over mTLS: restarted node$vid re-handshaked + converged to leader log (incl $EXTRA)";;
      *) fail "restarted node$vid caught up but $EXTRA not in committed log";;
    esac
  else
    echo "    node$vid: $(status_of "${APORTS[$VICTIM]}")"
    fail "restarted node$vid did not catch up over mTLS within deadline"
  fi
fi

echo
if [ "$PASS" = "1" ]; then
  echo "[prod_tls_test] ALL PASS — encrypted cluster works + plaintext/wrong-CA REFUSED + mTLS reconnect"
  exit 0
else
  echo "[prod_tls_test] FAILED"
  exit 1
fi
