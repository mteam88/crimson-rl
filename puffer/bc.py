"""Behavior cloning from TAS runs: the PPO policy's architecture trained to take the search's decisions.

Each run is the decisions file tas writes beside its transport (<transport>.actions: the seed, then CR_NUM_ATNS
int32 per decision; written for unfinished runs too). Replaying the decisions in the env regenerates the observations, so nothing large is stored. Lanes
play runs side by side and the recurrent policy trains on chunks of each, from a zero state as PPO's update does. The
value head learns each state's discounted return under the training reward. The result loads as a PPO starting
point: just train --load-model-path <out>.

Usage: .venv/bin/python puffer/bc.py <out.pt> <run.actions>... [--steps N] [--lanes B] [--chunk T]
"""
import argparse
import ctypes
import os
import random
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from configparser import ConfigParser
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "puffer"))
sys.path.insert(0, str(next((ROOT / "upstream").glob("PufferLib-*"))))

import pufferlib.models  # noqa: E402

from policy import CrimsonEncoder, L  # noqa: E402

HEADS = [L["CR_MOVE"], L["CR_AIM"], L["CR_FIRE"], L["CR_RELOAD"], L["CR_PERK"]]
OBS = L["CR_OBS_SIZE"]


class EnvConfig(ctypes.Structure):
    _fields_ = [("mode", ctypes.c_int), ("repeat", ctypes.c_int), ("max_ticks", ctypes.c_long),
                ("xp_scale", ctypes.c_float), ("death_penalty", ctypes.c_float), ("alive_reward", ctypes.c_float)]


def load_env_lib(ini):
    os.environ.setdefault("CRIMSON_CORE_SO", str(ROOT / "build/core/libcrimson_core.so"))
    lib = ctypes.CDLL(str(ROOT / "build/libcrimson_env.so"))
    lib.crimson_env_new.restype = ctypes.c_void_p
    lib.crimson_env_new.argtypes = [ctypes.POINTER(EnvConfig), ctypes.c_uint64, ctypes.c_char_p]
    lib.crimson_env_reset_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    lib.crimson_env_step.restype = ctypes.c_float
    lib.crimson_env_step.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    env = ini["env"]
    config = EnvConfig(int(env["mode"]), int(env["action_repeat"]), int(env["max_ticks"]),
                       int(env["xp_scale_milli"]) / 1000, int(env["death_penalty_milli"]) / 1000,
                       int(env["alive_reward_milli"]) / 1000)
    return lib, config


class Run:
    """A run to learn from: replaying `actions` from `seed` gives the observations, `targets` the decisions to learn
    (the actions themselves for a TAS run; the expert's labels for a DAgger rollout)."""

    def __init__(self, seed, actions, targets=None):
        self.seed, self.actions = seed, actions
        self.targets = actions if targets is None else targets
        self.returns = None

    @classmethod
    def load(cls, path):
        seed = struct.unpack_from("<I", Path(path).read_bytes())[0]
        return cls(seed, np.fromfile(path, dtype=np.int32, offset=4).reshape(-1, len(HEADS)))


class Lane:
    """One env replaying runs, a random one each time the last ends."""

    def __init__(self, lib, config, core, runs, index):
        self.lib, self.runs = lib, runs
        self.env = lib.crimson_env_new(ctypes.byref(config), 1000 + index, core)
        self.obs = np.zeros(OBS, dtype=np.float32)
        # Lanes start at random points of their first run, so a batch mixes stages of the game.
        run = random.choice(runs)
        self.start(run, random.randrange(len(run.actions)))

    def start(self, run, skip=0):
        self.run, self.t = run, 0
        self.lib.crimson_env_reset_seed(self.env, run.seed, self.obs.ctypes.data)
        done = ctypes.c_int(0)
        for self.t in range(skip):
            self.lib.crimson_env_step(self.env, run.actions[self.t].ctypes.data, self.obs.ctypes.data,
                                      ctypes.byref(done))
        self.t = skip

    def play(self, obs_out, act_out, ret_out):
        """Fills one chunk: the observation each decision was made on, the decision, its return."""
        done = ctypes.c_int(0)
        for k in range(len(obs_out)):
            if self.t >= len(self.run.actions):
                self.start(random.choice(self.runs))
            obs_out[k] = self.obs
            a = self.run.actions[self.t]
            act_out[k] = self.run.targets[self.t]
            ret_out[k] = self.run.returns[self.t]
            self.lib.crimson_env_step(self.env, a.ctypes.data, self.obs.ctypes.data, ctypes.byref(done))
            self.t += 1
            if done.value:
                self.t = len(self.run.actions)


def discounted_returns(lib, config, core, run, gamma):
    env = lib.crimson_env_new(ctypes.byref(config), 1, core)
    obs = np.zeros(OBS, dtype=np.float32)
    lib.crimson_env_reset_seed(env, run.seed, obs.ctypes.data)
    done = ctypes.c_int(0)
    rewards = []
    for a in run.actions:
        rewards.append(lib.crimson_env_step(env, a.ctypes.data, obs.ctypes.data, ctypes.byref(done)))
        if done.value:
            break
    lib.crimson_env_free(ctypes.c_void_p(env))
    run.actions, run.targets = run.actions[:len(rewards)], run.targets[:len(rewards)]
    g, out = 0.0, np.zeros(len(rewards), dtype=np.float32)
    for t in range(len(rewards) - 1, -1, -1):
        g = rewards[t] + gamma * g
        out[t] = g
    run.returns = out


def make_policy(ini, device):
    hidden = int(ini["policy"]["hidden_size"])
    network = pufferlib.models.MinGRU(hidden_size=hidden, num_layers=int(ini["policy"]["num_layers"]))
    policy = pufferlib.models.Policy(CrimsonEncoder(OBS, hidden), pufferlib.models.DefaultDecoder(HEADS, hidden),
                                     network)
    return policy.to(device)


class Trainer:
    """The policy, its optimizer, and lanes replaying `runs` (a list the caller may extend while training)."""

    def __init__(self, ini, runs, lanes=64, chunk=64, lr=3e-4, threads=6, value_coef=0.25, init=None, device="cuda"):
        self.lib, self.config = load_env_lib(ini)
        self.core = str(ROOT / "build/core/libcrimson_core.so").encode()
        self.gamma = float(ini["train"]["gamma"])
        self.runs, self.value_coef, self.device = runs, value_coef, device
        self.pool = ThreadPoolExecutor(threads)
        self.add(runs)
        self.lanes = None  # made on the first train(), once there are runs
        self.policy = make_policy(ini, device)
        if init:
            state_dict = torch.load(init, map_location=device)
            self.policy.load_state_dict({k.replace("module.", ""): v for k, v in state_dict.items()})
        self.opt = torch.optim.AdamW(self.policy.parameters(), lr=lr, weight_decay=1e-4)
        B, T = lanes, chunk
        self.obs = np.zeros((B, T, OBS), dtype=np.float32)
        self.act = np.zeros((B, T, len(HEADS)), dtype=np.int64)
        self.ret = np.zeros((B, T), dtype=np.float32)
        self.steps = 0

    def add(self, runs):
        """Computes the returns of new runs; then they may join self.runs."""
        list(self.pool.map(lambda run: discounted_returns(self.lib, self.config, self.core, run, self.gamma), runs))

    def train(self, steps, log_every=50):
        B, T = self.act.shape[:2]
        if self.lanes is None:
            self.lanes = list(self.pool.map(lambda i: Lane(self.lib, self.config, self.core, self.runs, i), range(B)))
        policy, t0 = self.policy, time.time()
        policy.train()
        for step in range(1, steps + 1):
            list(self.pool.map(lambda i: self.lanes[i].play(self.obs[i], self.act[i], self.ret[i]), range(B)))
            o = torch.from_numpy(self.obs).to(self.device)
            a = torch.from_numpy(self.act).to(self.device).view(B * T, -1)
            r = torch.from_numpy(self.ret).to(self.device)

            # Like PPO's update (MinGRU.forward_train), each chunk starts from a zero recurrent state.
            logits, values = policy(o)
            losses = [F.cross_entropy(lg, a[:, i]) for i, lg in enumerate(logits)]
            value_loss = F.smooth_l1_loss(values.reshape(-1), r.view(-1))
            loss = sum(losses) + self.value_coef * value_loss
            self.opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(policy.parameters(), 1.0)
            self.opt.step()
            self.steps += 1
            if step % log_every == 0:
                with torch.no_grad():
                    acc = [(lg.argmax(-1) == a[:, i]).float().mean().item() for i, lg in enumerate(logits)]
                print(f"step {self.steps}  loss {loss.item():.3f}  heads " + " ".join(f"{x.item():.3f}" for x in losses)
                      + "  acc " + " ".join(f"{x:.2f}" for x in acc) + f"  value {value_loss.item():.2f}"
                      + f"  {step * B * T / (time.time() - t0):.0f} samples/s", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("runs", nargs="+", help=".actions files")
    p.add_argument("--steps", type=int, default=20000)
    p.add_argument("--lanes", type=int, default=64)
    p.add_argument("--chunk", type=int, default=64)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--threads", type=int, default=6)
    p.add_argument("--value-coef", type=float, default=0.25)
    p.add_argument("--init", help="start from these weights")
    args = p.parse_args()

    ini = ConfigParser()
    ini.read(ROOT / "puffer/crimson.ini")
    runs = [Run.load(path) for path in args.runs]
    trainer = Trainer(ini, runs, args.lanes, args.chunk, args.lr, args.threads, args.value_coef, args.init)
    for run in runs:
        print(f"seed {run.seed}: {len(run.actions)} decisions, return {run.returns[0]:.0f}")
    for _ in range(0, args.steps, 500):
        trainer.train(min(500, args.steps - trainer.steps))
        torch.save(trainer.policy.state_dict(), args.out)


if __name__ == "__main__":
    main()
