#!/usr/bin/env bash
# The checks before posting a run: hill/verify.sh <run.bin>. The ranked check through upstream's WASM verifier, the
# .crd with the pilot fields, and the service's own upload path on it. Prints the ranked score last.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=$1 base=${1%.bin}
node core/ranked_check.mjs upstream/crimson/crimson-core/build/wasm/core.wasm < "$bin" > "$base.result.json"
upstream/crimson/.venv/bin/python tools/crd.py "$bin" "$base.crd" "$base.result.json" \
  --pilot-name "crimson-rl TAS" --pilot-model "TAS search" --pilot-url https://github.com/mteam88/crimson-rl
bun tools/service_check.ts "$PWD/upstream/crimson/service" "$base.crd" "$bin"
