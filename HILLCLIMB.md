# Hill-climb log

A 24-hour hill-climb on the TAS score, from 2026-10-08 23:46 EDT. Every run is on master rules (banteg/crimson
046cb811c, replay format 32).

## Rules

- Runs go through `hill/run.sh <name> <seed> <tas args>`, each in its own capped unit, about 3 at a time. The PowerSpec
  is shared, so leave the other jobs alone.
- A finished run goes through `hill/verify.sh build/hill/<name>.bin`: the ranked check, the `.crd` with pilot fields,
  then the service's upload path. A run that beats the current #1 and gets `verdict: ok` is posted as "mteam (TAS)"
  with `tools/submit.py`.
- TAS args after the out path: candidates, segment, lookahead, max minutes, energizer bars, grim minute, guard bars,
  patience, deep (see `tas/tas.cpp`).

## Best

61,725,602: seed 1, `64 8 120 0 0 10 0` (p1-g0), posted as
[6ec3a605…](https://crimson.land/runs/6ec3a605be1c9709888bebf7ff14e2d2462c533813c350fe928bc8d20018f0a6), #1 on the
bot board.

## Runs

| run | seed | args | died at | before Grim Deal | final | notes |
| --- | --- | --- | --- | --- | --- | --- |
| p1-g1 | 1 | `64 8 120 0 0 10 1` | 23.13 min | 50,286,440 | 59,493,594 | |
| p1-g0 | 1 | `64 8 120 0 0 10 0` | 27.01 min | 52,230,634 | 61,725,602 | posted |
| p2-g1 | 2 | `64 8 120 0 0 10 1` | | | | running |
| h1-s1-deep | 1 | `64 8 120 0 0 10 0 80 3` | | | | deep endgame search, patience 80 |
| h2-s3-deep | 3 | `64 8 120 0 0 10 0 80 3` | | | | |
| h3-s1-weapon2 | 1 | `64 8 120 0 0 10 0 80 3 2` | | | | top weapon worth 2 bars; A/B with h1 |

## Findings

- Score is about 0.32 × total damage dealt (creature health grows with experience), times Quick Learner, Double
  Experience and the final Grim Deal. Once the arena saturates (15 min) it is damage-limited, so a run's ceiling scales
  with damage per second.
- Only Instant Winner, Fatal Lottery and Random Weapon stack, so after about 50 perks every pick is one of those (or
  Grim Deal): keep the weapon or reroll it. In the 61.7M run the weapon changes every 5 to 10 seconds late on.
- `just weapons` at 23 to 25 minutes of the 61.7M run (3 s windows, 32 rollouts, relative to the median weapon):
  Splitter Gun 1.9 to 4.6, Ion Cannon 1.4 to 2.0, Rocket Launcher 1.0 to 1.8, Ion Shotgun 1.0 to 1.9; most others
  about 1. Noisy: at 23 minutes no weapon mattered.
- Living Fortress multiplies bullet damage by 1 + 0.05 × seconds standing still (up to 30): no use while dodging.

## Queue

1. Deep endgame search with patience (h1, h2).
2. Top weapons in the value (h3). Damage-perk priorities matter less: every one-shot perk is owned by about 10
   minutes either way.
3. Higher guard weights for chaining Shields.
4. A seed hunt with the best config.
