<img src="assets/icon.svg" width="64" height="64" alt="">

# crimson-rl

Tool-assisted and learned Survival runs for [banteg/crimson](https://github.com/banteg/crimson), the Crimsonland
rebuild, ranked on [crimson.land](https://crimson.land)'s bot board.

The core is upstream's crimson-core, recompiled as a shared library with swappable world state (`core/`), so a run can
be saved, restored and forked cheaply. On top of it:

- `env/`: a Survival environment (observations, actions, the perk menu as the game runs it) with a C API.
- `tas/`: the TAS. From the committed state it plays candidate plans of a scripted controller in parallel, each for a
  segment and then a lookahead, commits the best (survival first, then experience), and backs up when every candidate
  dies. Late in a run it holds a perk and finishes with Grim Deal (+18%).
- `puffer/`: PufferLib training, behavior cloning from TAS runs, and DAgger with the TAS search as the expert.
- `tools/`: transports to `.crd` replays (`crd.py`), submission (`submit.py`), video rendering.

Every run is checked the way crimson.land checks it: `core/ranked_check.mjs` replays the transport through upstream's
WASM verifier core and derives the result.

## Running

Needs clang, Node, uv, and Zig 0.17.0 for upstream's core. See the `justfile`:

```sh
just upstream-core        # clone banteg/crimson into upstream/ and build its native and WASM cores
just core                 # the world-swappable shared core
just tas 1 build/tas.bin  # a TAS run on seed 1, then the ranked check
just crd build/tas.bin build/tas.crd
```

Created in [T3 Code](https://t3.codes).
