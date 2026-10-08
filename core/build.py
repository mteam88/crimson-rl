"""Build crimson-core as a shared library whose game state can be swapped per world.

Upstream's `crimson-core/build.py` generates the adapted, rule-patched sources into
`upstream/crimson/crimson-core/build/native`. We recompile those same sources with
`-fPIC -fsemantic-interposition`, so every access to a global goes through the GOT,
and make host.cpp's file-scope state global so it does too. `world.cpp` then gives
each world its own copy of the state and points the GOT at it before stepping.
Nothing in the gameplay code changes; only where its globals live.
"""

import argparse
import concurrent.futures
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "upstream/crimson"
CORE = UPSTREAM / "crimson-core"
GEN = CORE / "build/native"

# Same as upstream build.py's native flags, plus PIC with interposable globals.
FLAGS = [
    "-g",
    "-std=c++17",
    "-fms-extensions",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-ffp-contract=off",
    "-O2",
    "-Wno-ignored-attributes",
    "-Wno-write-strings",
    "-Wno-address-of-temporary",
    "-Wno-deprecated-register",
    "-Wno-int-to-pointer-cast",
    "-I" + str(CORE / "host"),
    "-I" + str(GEN / "include"),
    "-I" + str(UPSTREAM / "third_party/headers"),
    "-fPIC",
    "-fsemantic-interposition",
]

# host.cpp file-scope state: `static T name...;` at column 0, no parentheses (not a function).
HOST_STATE = re.compile(r"^static (?!const\b)([A-Za-z_][\w:<>]*) ([^();=]+(\[[^\]]*\])?(, *[^();=]+)*);$", re.M)


def host_source(out: Path) -> Path:
    text = (CORE / "host/host.cpp").read_text()
    text, n = HOST_STATE.subn(r"\1 \2;", text)
    if n < 15:
        raise SystemExit(f"host.cpp: expected the file-scope state statics, matched {n}")
    # A static that shadowed a game global of the same name needs its own symbol once global.
    for name in ("creature_health",):
        text, k = re.subn(rf"^(float {name}\[)", rf"#define {name} host_{name}\n\1", text, flags=re.M)
        if k != 1:
            raise SystemExit(f"host.cpp: expected one definition of {name}, found {k}")
    dst = out / "host_world.cpp"
    dst.write_text(f'#line 1 "{CORE / "host/host.cpp"}"\n' + text)
    return dst


def compile_one(src: Path, out: Path) -> str | None:
    obj = out / (src.stem + ".o")
    proc = subprocess.run(["clang++", *FLAGS, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    return f"{src}\n{proc.stderr}" if proc.returncode else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, default=ROOT / "build/core")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    if not (GEN / "data.cpp").exists():
        raise SystemExit("run upstream's build first: cd upstream/crimson && uv run python crimson-core/build.py")
    sources = sorted(s for s in GEN.glob("*.cpp")) + [host_source(a.out)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        errors = [e for e in pool.map(lambda s: compile_one(s, a.out), sources) if e]
    if errors:
        print("\n".join(errors)[:8000])
        raise SystemExit(f"{len(errors)} of {len(sources)} failed")
    objs = [str(a.out / (s.stem + ".o")) for s in sources] + [str(GEN / "math.o")]
    lib = a.out / "libcrimson_core.so"
    # norelro keeps the GOT writable; world.cpp rewrites its state slots.
    subprocess.run(["clang++", "-shared", "-Wl,-z,norelro", "-Wl,-z,now", *objs, "-o", str(lib)], check=True)
    print(f"{len(sources)} sources -> {lib}")


if __name__ == "__main__":
    main()
