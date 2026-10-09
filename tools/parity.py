"""The first tick where upstream's Python sim and the WASM verifier core disagree on a run.

tools/parity.py <run.crd> <run.bin>, with upstream's venv. `just render` plays the Python sim, so a run that
diverges renders wrong from that tick on.
"""

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "upstream/crimson/crimson-core/checks"))
import gate  # noqa: E402

from crimson.replay.codec import load_replay_file  # noqa: E402
from crimson.replay.driver.playback_driver import PlaybackDriver  # noqa: E402

crd, run = sys.argv[1], sys.argv[2]
names = [core for _, core, _, _ in gate.FIELDS]
with tempfile.NamedTemporaryFile() as out:
    subprocess.run(
        ["node", str(ROOT / "tools/parity_dump.mjs"), str(gate.CORE / "build/wasm/core.wasm"), run, json.dumps(names), out.name],
        check=True,
    )
    data = Path(out.name).read_bytes()
k = len(names)
rows = [struct.unpack_from(f"<{k}I", data, i * 4 * k) for i in range(len(data) // (4 * k))]
driver = PlaybackDriver(load_replay_file(crd), version_mismatch_action=None)
session = driver.session
for t, row in enumerate(rows):
    if t:
        driver.step_tick(t - 1)
    diff = {
        label: (gate._from_bits(gate._bits(get(session), f32), f32), gate._from_bits(row[i], f32))
        for i, (label, _, get, f32) in enumerate(gate.FIELDS)
        if gate._bits(get(session), f32) != row[i]
    }
    if diff:
        print(f"diverges at tick {t - 1}: {diff} (python, core)")
        sys.exit(1)
print(f"agree for all {len(rows) - 1} ticks")
