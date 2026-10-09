"""Plays a policy checkpoint live (no search) on fixed seeds and prints each run's experience: the RL bot's score,
comparable with tas/bot.cpp's scripted baseline on the same seeds.

Usage: .venv/bin/python puffer/evaluate.py <weights.pt|.bin> [--first-seed 1000] [--runs 32] [--greedy]
"""
import argparse
import ctypes
import sys
from configparser import ConfigParser
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bc import HEADS, OBS, ROOT, load_env_lib, make_policy  # noqa: E402


class Stats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_double) for name in
                ("runs", "score", "ticks", "episode_return", "perks", "reveal_failed", "game_errors",
                 "picks_deferred")]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("weights")
    p.add_argument("--first-seed", type=int, default=1000)
    p.add_argument("--runs", type=int, default=32)
    p.add_argument("--greedy", action="store_true", help="take each head's likeliest action instead of sampling")
    args = p.parse_args()

    ini = ConfigParser()
    ini.read(ROOT / "puffer/crimson.ini")
    lib, config = load_env_lib(ini)
    lib.crimson_env_take_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(Stats)]
    core = str(ROOT / "build/core/libcrimson_core.so").encode()
    device = "cuda"
    policy = make_policy(ini, device)
    state_dict = torch.load(args.weights, map_location=device)
    policy.load_state_dict({k.replace("module.", ""): v for k, v in state_dict.items()})
    policy.eval()

    n = args.runs
    envs = [lib.crimson_env_new(ctypes.byref(config), 1, core) for _ in range(n)]
    obs = np.zeros((n, OBS), dtype=np.float32)
    for i, env in enumerate(envs):
        lib.crimson_env_reset_seed(env, args.first_seed + i, obs[i].ctypes.data)
    state = policy.initial_state(n, device)
    live = np.ones(n, dtype=bool)
    results = {}
    done = ctypes.c_int(0)
    stats = Stats()
    with torch.no_grad():
        while live.any():
            logits, _, state = policy.forward_eval(torch.from_numpy(obs).to(device), state)
            if args.greedy:
                acts = [lg.argmax(-1) for lg in logits]
            else:
                acts = [torch.multinomial(lg.softmax(-1), 1).squeeze(-1) for lg in logits]
            acts = torch.stack(acts, -1).int().cpu().numpy()
            for i in np.flatnonzero(live):
                lib.crimson_env_step(envs[i], acts[i].ctypes.data, obs[i].ctypes.data, ctypes.byref(done))
                if done.value:
                    lib.crimson_env_take_stats(envs[i], ctypes.byref(stats))
                    live[i] = False
                    results[i] = (int(stats.score), stats.ticks / 3600)
                    print(f"seed {args.first_seed + i}: experience {results[i][0]}, {results[i][1]:.2f} minutes",
                          flush=True)
    xp = sorted(r[0] for r in results.values())
    print(f"{n} runs: mean {np.mean(xp):.0f}, median {xp[n // 2]}, best {xp[-1]}, worst {xp[0]}")


if __name__ == "__main__":
    main()
