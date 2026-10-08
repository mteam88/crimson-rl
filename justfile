core_host := "upstream/crimson/crimson-core/host"
cxx := "clang++ -O2 -g -std=c++17 -I core -I " + core_host + " -I upstream/crimson/third_party/headers -I upstream/crimson/crimson-core/build/native/include"

# Clone banteg/crimson into upstream/ (gitignored) if missing.
upstream:
    [ -d upstream/crimson ] || git clone https://github.com/banteg/crimson upstream/crimson

# Upstream's native core (generates the adapted sources we recompile) and the WASM verifier core
# crimson.land runs, our reference. Needs Zig 0.17.0.
upstream-core: upstream
    cd upstream/crimson && uv sync -q && uv run python crimson-core/build.py && uv run python crimson-core/build.py --target wasm

# The world-swappable shared core.
core:
    uv run --no-project python core/build.py

world-check: core
    {{cxx}} core/world.cpp core/world_check.cpp -ldl -pthread -o build/world_check
    ./build/world_check build/core/libcrimson_core.so "node core/wasm_snapshots.mjs upstream/crimson/crimson-core/build/wasm/core.wasm"

env-check threads="4" envs="16" seconds="5": core
    {{cxx}} -fms-extensions -I env core/world.cpp env/env.cpp env/env_check.cpp -ldl -pthread -o build/env_check
    ./build/env_check build/core/libcrimson_core.so "node core/wasm_snapshots.mjs upstream/crimson/crimson-core/build/wasm/core.wasm" {{threads}} {{envs}} {{seconds}}
