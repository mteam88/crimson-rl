# Hill-climb log

A 24-hour hill-climb on the TAS score, from 2026-10-08 23:46 EDT. Runs up to h5 are on banteg/crimson 046cb811c;
from 2026-10-09 12:50 EDT runs are on v0.14.0 (020068bb1, replay format 32).

v0.14.0 counts experience past 2^24 exactly (banteg/crimson#600), so every run that passed 16.7M XP under the old
core replays differently and is retired from the boards. p2-g1 under v0.14 sends an illegal pick at tick 97,782 once
its XP has drifted. Our env already follows the v0.14 perk-menu rule (open, then pick on the next tick), so the TAS
needs only the rebuilt core. h1, h3, h4 and h5 were stopped unfinished.

## Rules

- Runs go through `hill/run.sh <name> <seed> <tas args>`, each in its own capped unit, about 3 at a time. The PowerSpec
  is shared, so leave the other jobs alone.
- A finished run goes through `hill/verify.sh build/hill/<name>.bin`: the ranked check, the `.crd` with pilot fields,
  then the service's upload path. A run that beats the current #1 and gets `verdict: ok` is posted as "mteam (TAS)"
  with `tools/submit.py`.
- TAS args after the out path: candidates, segment, lookahead, max minutes, energizer bars, grim minute, guard bars,
  patience, deep (see `tas/tas.cpp`).

## Best

99,424,354: seed 2, `64 8 120 0 0 10 1` (p2-g1), posted 2026-10-09 01:36 EDT as
[5f05d70b…](https://crimson.land/runs/5f05d70bcf860f5292c2de2706fafb2ca127c4571df5efc98b19957b79029c75), #1 on the
bot board. Before it: 61,725,602 (p1-g0,
[6ec3a605…](https://crimson.land/runs/6ec3a605be1c9709888bebf7ff14e2d2462c533813c350fe928bc8d20018f0a6)).

## Runs

| run | seed | args | died at | before Grim Deal | final | notes |
| --- | --- | --- | --- | --- | --- | --- |
| p1-g1 | 1 | `64 8 120 0 0 10 1` | 23.13 min | 50,286,440 | 59,493,594 | |
| p1-g0 | 1 | `64 8 120 0 0 10 0` | 27.01 min | 52,230,634 | 61,725,602 | posted |
| p2-g1 | 2 | `64 8 120 0 0 10 1` | 32.19 min | 84,236,426 | 99,424,354 | posted |
| h1-s1-deep | 1 | `64 8 120 0 0 10 0 80 3` | | | | deep endgame search, patience 80; stopped |
| h2-s3-deep | 3 | `64 8 120 0 0 10 0 80 3` | 35.45 min | 81,035,460 | 95,705,430 | |
| h3-s1-weapon2 | 1 | `64 8 120 0 0 10 0 80 3 2` | | | | top weapon worth 2 bars; A/B with h1; stopped |
| h4-s2-deep | 2 | `64 8 120 0 0 10 1 80 3` | | | | p2-g1 with the deep endgame search; stopped |
| h5-s4-deep | 4 | `64 8 120 0 0 10 1 80 3` | | | | stopped |
| v14-s2 | 2 | `64 8 120 0 0 10 1` | | | | v0.14 rerun of p2-g1 |
| v14-s3 | 3 | `64 8 120 0 0 10 1` | 19.47 min | 18,485,172 | 21,974,192 | v0.14; verdict ok, not posted (bot #1 is egornomic 143.8M) |
| v14-s2-deep | 2 | `64 8 120 0 0 10 1 80 3` | | | | v0.14 |

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
