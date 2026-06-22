#!/usr/bin/env bash
# Bounded TLC runner.
#
# Backprop: a bare `java ... tlc2.TLC` run dumped a ~15 GB state queue into
# states/ in the repo CWD (10k+ files) and can grab unbounded heap -> disk +
# memory pressure that froze the host. This wrapper makes TLC safe to run:
#   - heap capped (-Xmx) so it can't eat all RAM;
#   - scratch/metadir in /tmp (auto-removed on exit) so it NEVER writes the repo;
#   - workers bounded so it doesn't peg every core.
#
# Usage: scripts/tlc.sh -config specs/Consensus.cfg specs/Consensus.tla
# Env:   TLC_XMX (default 4g), TLC_WORKERS (default 2)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JAR="$ROOT/tools/tla/tla2tools.jar"
[ -f "$JAR" ] || { echo "tla2tools.jar not found at $JAR" >&2; exit 1; }

XMX="${TLC_XMX:-4g}"
WORKERS="${TLC_WORKERS:-2}"
META="$(mktemp -d /tmp/lockstep-tlc.XXXXXX)"
trap 'rm -rf "$META"' EXIT

ulimit -c 0 2>/dev/null || true   # no core dumps

exec java "-Xmx${XMX}" -XX:+UseParallelGC -cp "$JAR" tlc2.TLC \
  -workers "$WORKERS" -metadir "$META" "$@"
