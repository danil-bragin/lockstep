#!/usr/bin/env bash
# Run the FULL universal merge gate inside a Linux container — the stages macOS
# can't do (MSan, clang-tidy, scan-build) run for real here. arm64-native.
#
# Usage: scripts/gate-linux.sh [extra args passed to gate.sh]
# The repo is mounted at /work; the container builds into ./build (gitignored).
# A fresh build is forced so Linux objects never mix with macOS objects.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="lockstep-dev"

echo "==> building Linux dev image ($IMG) ..."
docker build -t "$IMG" -f "$ROOT/tools/docker/Dockerfile" "$ROOT/tools/docker"

echo "==> running the full gate inside Linux ..."
docker run --rm \
  -v "$ROOT:/work" -w /work \
  --memory=8g --cpus=6 \
  "$IMG" bash -lc 'rm -rf build && ulimit -c 0 && bash scripts/gate.sh'
