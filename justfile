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
    ./build/env_check build/core/libcrimson_core.so "node core/wasm_snapshots.mjs upstream/crimson/crimson-core/build/wasm/core.wasm" "node core/ranked_check.mjs upstream/crimson/crimson-core/build/wasm/core.wasm" {{threads}} {{envs}} {{seconds}}

# A tool-assisted run on `seed` (tas/tas.cpp; args: candidates segment lookahead max-minutes), then the
# leaderboard's ranked check of the transport it wrote.
tas seed out="build/tas.bin" *args: core
    {{cxx}} -fms-extensions -fopenmp -I env core/world.cpp env/env.cpp tas/tas.cpp -ldl -pthread -o build/tas
    systemd-run --user --scope --unit=crimson-tas-$(date +%s%N) -p MemoryHigh=12G ./build/tas build/core/libcrimson_core.so {{seed}} {{out}} {{args}}
    node core/ranked_check.mjs upstream/crimson/crimson-core/build/wasm/core.wasm < {{out}}

# A transport as a .crd replay (tools/crd.py), then upstream's own verifier on it.
crd transport out:
    upstream/crimson/.venv/bin/python tools/crd.py {{transport}} {{out}}
    upstream/crimson/.venv/bin/crimson replay verify {{out}}

puffer_rev := "42f70d6932c30ac977736f861006809c50168ba9"

# PufferLib 4.0 at the revision bopl pins, and a venv with its locked toolchain (torch, CUDA 13 wheels).
puffer-setup:
    [ -d upstream/PufferLib-{{puffer_rev}} ] || (curl -sL https://codeload.github.com/PufferAI/PufferLib/tar.gz/{{puffer_rev}} -o upstream/pufferlib.tar.gz && echo "c8d8b81a6812854e17f86ade232b55d8069871ab17b59ceed405158b371a2845  upstream/pufferlib.tar.gz" | sha256sum -c && tar xzf upstream/pufferlib.tar.gz -C upstream && rm upstream/pufferlib.tar.gz)
    [ -x .venv/bin/python ] || uv venv --python 3.11 .venv
    uv pip install --python .venv/bin/python -r puffer/requirements.lock
    uv pip install --python .venv/bin/python --no-deps --no-build-isolation -e upstream/PufferLib-{{puffer_rev}}

# PufferLib's _C with the env linked in.
puffer: core
    .venv/bin/python puffer/build.py

train *args: puffer
    systemd-run --user --scope --unit=crimson-train-$(date +%s) -p MemoryHigh=12G .venv/bin/python puffer/train.py train {{args}}

# A .crd replay as video, through upstream's renderer (opens a window); tools/ffmpeg-nvenc stands in for an
# ffmpeg without libx264.
render replay out *args:
    upstream/crimson/.venv/bin/crimson replay render {{replay}} --out {{out}} --ffmpeg-bin tools/ffmpeg-nvenc --overwrite {{args}}

# The env and the DAgger expert as a shared library (env/capi.h), for Python tools: puffer/bc.py, dagger.py.
env-lib: core
    {{cxx}} -fms-extensions -fopenmp -fPIC -shared -I env -I tas core/world.cpp env/env.cpp env/capi.cpp -ldl -pthread -o build/libcrimson_env.so

# Behavior cloning from TAS runs (transports with their .actions): a PPO starting point.
bc out *args: env-lib
    systemd-run --user --scope --unit=crimson-bc-$(date +%s) -p MemoryHigh=12G .venv/bin/python puffer/bc.py {{out}} {{args}}

# The scripted controller playing live from `runs` seeds (no search): a baseline.
bot first runs="32" *args: core
    {{cxx}} -fms-extensions -fopenmp -I env core/world.cpp env/env.cpp tas/bot.cpp -ldl -pthread -o build/bot
    systemd-run --user --scope --unit=crimson-bot-$(date +%s) -p MemoryHigh=8G ./build/bot build/core/libcrimson_core.so {{first}} {{runs}} {{args}}
