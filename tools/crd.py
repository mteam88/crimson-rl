"""Turns a transport (upstream crimson-core's input stream, as env/tas recordings write it) into a .crd replay, the
file crimson.land takes. The run must use the ranked Survival profile. Its result is derived by upstream's own
Python simulation (not the core we recorded with), then `crimson replay verify` can check the file independently; or,
given `result.json` (core/ranked_check.mjs's output), it is the WASM core's, which is what crimson.land checks against.
Long runs need the latter: the Python simulation drifts from the WASM core (RNG draws, XP rounding past 2^24).

Run with upstream's environment: upstream/crimson/.venv/bin/python tools/crd.py <transport> <out.crd> [result.json]
"""
import json
import platform
import struct
import subprocess
import sys
from pathlib import Path

from crimson.game_modes import GameMode
from crimson.replay.codec import dump_replay_file
from crimson.replay.driver.playback_driver import build_verify_playback_driver
from crimson.replay.ranked import ranked_run_spec
from crimson.replay.recorder import ReplayRecorder
from crimson.replay.types import Recorder, ReplayTick
from crimson.sim.commands import PerkMenuOpenCommand, PerkPickCommand
from crimson.sim.run_result import PlayerRunResult, RunOutcome, RunResult

CONFIG_WORDS = 65


def config_words(run):
    """The transport config a RunSpec encodes to (upstream service/src/transport.ts encodeTransport)."""
    q = run.quest_level
    return [run.seed, run.game_mode_id, q.major if q else 1, q.minor if q else 1, run.status.quest_unlock_index,
            run.status.quest_unlock_index_hardcore, run.detail_preset, run.violence_disabled, int(run.friendly_fire),
            int(run.hardcore), run.quest_fail_retry_count, int(run.preserve_bugs), *run.status.weapon_usage_counts]


def main(src, out, derived=None):
    data = Path(src).read_bytes()
    words = list(struct.unpack_from(f"<{CONFIG_WORDS}I", data))
    run = ranked_run_spec(GameMode.SURVIVAL, seed=words[0])
    if words != config_words(run):
        sys.exit("the transport's settings are not the ranked Survival profile")

    root = Path(__file__).resolve().parents[1]
    sha = subprocess.run(["git", "-C", root, "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True)
    recorder = ReplayRecorder(run)
    recorder._recorder = Recorder(client="crimson-rl", version=f"0.1.0+g{sha.stdout.strip() or 'unknown'}",
                                  platform=f"{sys.platform}-{platform.machine()}")
    at = CONFIG_WORDS * 4
    while at < len(data):
        mx, my, ax, ay, flags, count = struct.unpack_from("<4f2I", data, at)
        at += 24
        commands = []
        for _ in range(count):
            kind, arg = struct.unpack_from("<2i", data, at)
            at += 8
            commands.append(PerkPickCommand(player_index=0, choice_index=arg) if kind == 1
                            else PerkMenuOpenCommand(player_index=0))
        recorder.record(ReplayTick(inputs=[[mx, my, ax, ay, flags]], commands=commands))

    if derived:
        r = json.loads(Path(derived).read_text())["result"]
        result = RunResult(outcome=RunOutcome(r["outcome"]), elapsed_ms=r["elapsed_ms"], kills=r["kills"],
                           shots_fired=r["shots_fired"], shots_hit=r["shots_hit"], rng_state=r["rng_state"],
                           pending_perks=r["pending_perks"], quest_final_ms=None,
                           players=(PlayerRunResult(experience=r["experience"], health=float(r["health"]),
                                                    most_used_weapon_id=r["most_used_weapon_id"]),))
    else:
        # Derive the result by simulating, then record it.
        placeholder = RunResult(outcome=RunOutcome.INCOMPLETE, elapsed_ms=0, kills=0, shots_fired=0, shots_hit=0,
                                rng_state=0, pending_perks=0, quest_final_ms=None, players=())
        result = build_verify_playback_driver(recorder.finish(placeholder)).run()
    replay = recorder.finish(result)
    dump_replay_file(Path(out), replay)
    p = result.players[0]
    print(f"{out}: seed {run.seed}, {len(replay.ticks)} ticks, {result.outcome}, experience {p.experience}, "
          f"{result.kills} kills, game_version {replay.game_version}")


if __name__ == "__main__":
    main(*sys.argv[1:])
