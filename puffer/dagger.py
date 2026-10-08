"""DAgger with the TAS search as the expert: the policy plays live from fresh seeds, the search labels every state it
visits with the decision it would make there, and the policy trains on all labels so far (plus any TAS runs), so it
learns to recover from its own mistakes, which behavior cloning alone never shows it. The search only teaches; the
policy plays without it.

Each round: `lanes` envs play `decisions` steps each with the policy (a run that ends starts a new seed), every
state labeled; then `steps` training steps over everything. Rollouts are kept as (seed, decisions, labels), and
replaying them regenerates the observations.

Usage: .venv/bin/python puffer/dagger.py <out dir> [--init weights] [--tas build/bc/*.actions] [--rounds N] ...
"""
import argparse
import ctypes
import random
import sys
import time
from configparser import ConfigParser
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bc import HEADS, OBS, ROOT, Run, Trainer  # noqa: E402


def collect(trainer, expert, args, next_seed):
    """One round of rollouts: returns the finished and unfinished runs, each with its labels."""
    lib, policy, device = trainer.lib, trainer.policy, trainer.device
    n = args.rollout_lanes
    envs = [lib.crimson_env_new(ctypes.byref(trainer.config), 1, trainer.core) for _ in range(n)]
    obs = np.zeros((n, OBS), dtype=np.float32)
    seeds = [next_seed() for _ in range(n)]
    for i, env in enumerate(envs):
        lib.crimson_env_reset_seed(env, seeds[i], obs[i].ctypes.data)
    acts = [[] for _ in range(n)]
    labels = [[] for _ in range(n)]
    runs = []
    state = policy.initial_state(n, device)
    label = np.zeros(len(HEADS), dtype=np.int32)
    done = ctypes.c_int(0)
    deaths = 0
    policy.eval()
    with torch.no_grad():
        for step in range(args.decisions):
            logits, _, state = policy.forward_eval(torch.from_numpy(obs).to(device), state)
            sampled = torch.stack([torch.multinomial(lg.softmax(-1), 1).squeeze(-1) for lg in logits], -1)
            sampled = sampled.int().cpu().numpy()
            for i in range(n):
                died = lib.crimson_expert_label(expert, envs[i], args.segment, args.lookahead, step,
                                                label.ctypes.data)
                deaths += died >= 0
                # With probability beta the expert's decision is played, else the policy's.
                a = label.copy() if random.random() < args.beta else sampled[i].copy()
                acts[i].append(a)
                labels[i].append(label.copy())
                lib.crimson_env_step(envs[i], a.ctypes.data, obs[i].ctypes.data, ctypes.byref(done))
                if done.value:
                    runs.append(Run(seeds[i], np.array(acts[i]), np.array(labels[i])))
                    seeds[i] = next_seed()
                    acts[i], labels[i] = [], []
                    lib.crimson_env_reset_seed(envs[i], seeds[i], obs[i].ctypes.data)
                    state[0][:, i] = 0
    for i in range(n):
        if acts[i]:
            runs.append(Run(seeds[i], np.array(acts[i]), np.array(labels[i])))
        lib.crimson_env_free(ctypes.c_void_p(envs[i]))
    return runs, deaths


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out", help="directory for checkpoints and rollouts")
    p.add_argument("--init", help="starting weights (PPO or BC)")
    p.add_argument("--tas", nargs="*", default=[], help="TAS .actions files to learn from too")
    p.add_argument("--rounds", type=int, default=100)
    p.add_argument("--rollout-lanes", type=int, default=32)
    p.add_argument("--decisions", type=int, default=600, help="decisions per lane per round")
    p.add_argument("--steps", type=int, default=300, help="training steps per round")
    p.add_argument("--beta", type=float, default=0.0, help="chance the expert's decision is played in rollouts")
    p.add_argument("--candidates", type=int, default=16)
    p.add_argument("--segment", type=int, default=8)
    p.add_argument("--lookahead", type=int, default=60)
    p.add_argument("--lanes", type=int, default=64)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--first-seed", type=int, default=100000)
    args = p.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ini = ConfigParser()
    ini.read(ROOT / "puffer/crimson.ini")
    runs = [Run.load(path) for path in args.tas]
    seed = [args.first_seed]

    def next_seed():
        seed[0] += 1
        return seed[0]

    trainer = Trainer(ini, runs, args.lanes, 64, args.lr, args.threads, init=args.init)
    expert = make_expert(trainer, args)
    for r in range(1, args.rounds + 1):
        t0 = time.time()
        new, deaths = collect(trainer, expert, args, next_seed)
        trainer.add(new)
        finished = [len(run.actions) for run in new]
        labels = sum(finished)
        runs.extend(new)
        save_rollouts(out / f"round-{r:04d}.npz", new)
        t1 = time.time()
        trainer.train(args.steps, log_every=args.steps)
        torch.save(trainer.policy.state_dict(), out / "latest.pt")
        if r % 10 == 0:
            torch.save(trainer.policy.state_dict(), out / f"round-{r:04d}.pt")
        print(f"round {r}: {labels} labels in {t1 - t0:.0f}s ({labels / (t1 - t0):.0f}/s), {len(new)} runs, "
              f"expert saw death in {deaths / max(labels, 1):.1%}; {sum(len(x.actions) for x in runs)} labels total; "
              f"trained {time.time() - t1:.0f}s", flush=True)


def make_expert(trainer, args):
    lib = trainer.lib
    lib.crimson_expert_new.restype = ctypes.c_void_p
    lib.crimson_expert_new.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.crimson_expert_label.restype = ctypes.c_int
    lib.crimson_expert_label.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_long,
                                         ctypes.c_void_p]
    return lib.crimson_expert_new(ctypes.byref(trainer.config), args.candidates)


def save_rollouts(path, runs):
    np.savez_compressed(path, seeds=np.array([r.seed for r in runs], dtype=np.uint32),
                        lengths=np.array([len(r.actions) for r in runs]),
                        actions=np.concatenate([r.actions for r in runs]),
                        labels=np.concatenate([r.targets for r in runs]))


if __name__ == "__main__":
    main()
