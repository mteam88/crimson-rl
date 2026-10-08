// The scripted controller tas/tas.cpp searches over and tas/bot.cpp plays: flee danger, aim at a nearby creature,
// fire, take perks (never a fatal one).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

#include "layout.h"

namespace crimson {

constexpr float PI = 3.14159265358979f;
using Action = std::array<int, CR_NUM_ATNS>;

inline uint64_t mix(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// How a candidate moves, aims and picks perks for a while.
enum Move { FLEE = -1, STRAFE_LEFT = -2, STRAFE_RIGHT = -3 };  // >= 0: a fixed move head value
struct Plan {
  int move = FLEE;
  int aim_rank = 0;   // aim at the k-th nearest creature
  int perk = -1;      // choice index to take, -1 random
  float jitter = 0;   // chance per decision of a random move instead
};

struct Rng {
  uint64_t s;
  uint32_t next() { return (uint32_t)(mix(s += 0x9e3779b97f4a7c15ull) >> 32); }
  float unit() { return next() / 4294967296.0f; }
};

// Perks whose death comes later than any lookahead sees: Grim Deal kills on pick, after its experience lands;
// Death Clock kills 30 seconds later, invulnerable until then.
inline bool fatal_perk(int id) { return id == 0x08 || id == 0x2F; }

inline int move_head(float angle) {
  int k = (int)lroundf(angle / (2 * PI) * (CR_MOVE - 1));
  return 1 + ((k % (CR_MOVE - 1)) + (CR_MOVE - 1)) % (CR_MOVE - 1);
}

// The plan's action for the state in `obs`.
inline Action act(const Plan &plan, const float *obs, Rng &rng) {
  Action a{};
  const float *s = obs + CR_OFF_SCALARS;
  const float *c = obs + CR_OFF_CREATURES;
  // Danger: away from nearby creatures and hostile shots, weighted by closeness, and off the walls.
  float ax = 0, ay = 0;
  for (int k = 0; k < CR_CREATURES && c[k * CR_CREATURE_F] > 0; ++k) {
    const float *r = c + k * CR_CREATURE_F;
    float d = r[3];
    if (d > 0.7f) break;
    float w = 1 / (d * d + 0.004f);
    ax -= r[4] * w, ay -= r[5] * w;
  }
  const float *sh = obs + CR_OFF_SHOTS;
  for (int k = 0; k < CR_SHOTS && sh[k * CR_SHOT_F] > 0 && sh[k * CR_SHOT_F + 7] > 0; ++k) {
    const float *r = sh + k * CR_SHOT_F;
    float d = r[3];
    if (d > 0.4f) break;
    float w = 0.5f / (d * d + 0.004f);
    float len = sqrtf(r[1] * r[1] + r[2] * r[2]) + 1e-6f;
    ax -= r[1] / len * w, ay -= r[2] / len * w;
  }
  float px = s[0] * 1024, py = s[1] * 1024;
  auto wall = [](float gap) { return gap < 160 ? 40.0f * (160 - gap) / 160 : 0.0f; };
  float danger = sqrtf(ax * ax + ay * ay);
  ax += wall(px) - wall(1024 - px), ay += wall(py) - wall(1024 - py);
  if (danger < 1e-3f) ax += (512 - px) / 512, ay += (512 - py) / 512;  // nothing near: drift to the middle

  float flee = atan2f(ay, ax);
  if (plan.move >= 0) a[0] = plan.move;
  else if (plan.move == FLEE) a[0] = move_head(flee);
  else a[0] = move_head(flee + (plan.move == STRAFE_LEFT ? 1 : -1) * 0.45f * PI);
  if (plan.jitter > 0 && rng.unit() < plan.jitter) a[0] = rng.next() % CR_MOVE;

  int k = c[0] > 0 ? plan.aim_rank : -1;
  while (k > 0 && c[k * CR_CREATURE_F] <= 0) --k;
  if (k >= 0) {
    float ang = atan2f(c[k * CR_CREATURE_F + 2], c[k * CR_CREATURE_F + 1]);
    int h = (int)lroundf(ang / (2 * PI) * CR_AIM);
    a[1] = (h % CR_AIM + CR_AIM) % CR_AIM;
  }
  a[2] = 1;
  bool pending = s[36] > 0, revealed = s[37] > 0;
  int n = (int)lroundf(s[38] * 7);
  if (pending && !revealed) a[4] = 1;
  if (pending && revealed) {
    const float *choices = obs + CR_OFF_IDS + 2;
    int want = plan.perk >= 0 ? plan.perk % n : (int)(rng.next() % n);
    a[4] = 2 + want;
    for (int j = 0; j < n; ++j)
      if (!fatal_perk((int)choices[(want + j) % n])) {
        a[4] = 2 + (want + j) % n;
        break;
      }
  }
  return a;
}

}  // namespace crimson
