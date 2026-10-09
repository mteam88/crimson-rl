#!/usr/bin/env bash
# A hill-climb TAS run in its own capped unit: hill/run.sh <name> <seed> [tas args after the out path...]
# Writes build/hill/<name>.bin and logs/hill/<name>.log.
set -euo pipefail
cd "$(dirname "$0")/.."
name=$1 seed=$2; shift 2
mkdir -p build/hill logs/hill
# A rebuild of build/tas or the core must not change a running run.
cp build/tas "build/hill/$name.tas"; cp build/core/libcrimson_core.so "build/hill/$name.so"
systemd-run --user --unit="crimson-hill-$name" --collect -p MemoryHigh=4G -p Nice=5 \
  -E OMP_NUM_THREADS="${THREADS:-4}" -d \
  bash -c "exec build/hill/$name.tas build/hill/$name.so $seed build/hill/$name.bin $* > logs/hill/$name.log 2>&1"
