// One step of the TAS search, shared by tas/tas.cpp and the DAgger expert (env/capi.cpp): from a state, play
// candidate plans in parallel, each for a segment and then a lookahead under a default plan, and pick the best
// (survival first, then experience and health).
#pragma once
#include <omp.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "env.hpp"
#include "policy.hpp"

namespace crimson {

struct Outcome {
  int died_at;  // decision the run ended in, or -1
  int xp;       // experience gained
  float health;
  float crowd;  // creatures within 256 units at the end
  bool energizer;  // an Energizer is running or lies on the ground at the end
  std::vector<Action> segment;
  uint64_t at_k = 0;  // the world's hash after the segment, to check the commit reproduces it
  int candidate = 0;
};

// Survival first (later death is better); then experience and health, less crowding. A full bar is worth 2000
// experience, plus a quarter of the best candidate's gain so it still counts late, when experience comes in floods;
// each creature left within 256 units costs 2% of a bar. An Energizer, rarely dropped (1 bonus in 10368), is worth
// `energizer` bars when asked for.
inline double value(const Outcome &o, int best_xp, float energizer) {
  if (o.died_at >= 0) return -1e9 + o.died_at;
  double bar = 2000 + 0.25 * best_xp;
  return o.xp + (o.health / 100 - 0.02 * o.crowd + (o.energizer ? energizer : 0)) * bar;
}

// Whether an Energizer is running or lies on the ground in `obs`.
inline bool energizer_seen(const float *obs) {
  if (obs[CR_OFF_SCALARS + 34] > 0) return true;
  for (int k = 0; k < CR_BONUSES && obs[CR_OFF_BONUSES + k * CR_BONUS_F] > 0; ++k)
    if (obs[CR_OFF_BONUSES + k * CR_BONUS_F + 8] == BONUS_ENERGIZER) return true;
  return false;
}

class Search {
 public:
  float energizer = 0;  // value() weight of an Energizer; 0 leaves it to chance

  Search(const EnvConfig &cfg, int candidates)
      : M(candidates), obs_(CR_OBS_SIZE), obs_k_(M, std::vector<float>(CR_OBS_SIZE)) {
    for (int i = 0; i < M; ++i) envs_.push_back(std::make_unique<Env>(cfg, 2 + i));
  }

  // The best of the candidates from `cur`, segment K decisions then a lookahead of L (3L when a perk may be
  // taken). seed, segment and salt only vary the candidates. Adds the decisions played to *simulated.
  Outcome best(Env &cur, uint32_t seed, long segment, int salt, int K, int L, long *simulated) {
    std::vector<Plan> plans(M);
    for (int i = 0; i < M; ++i) {
      Rng r{mix(seed ^ mix(segment * 1000003 + salt * 7919 + i))};
      Plan &p = plans[i];
      if (i == 0) continue;  // the default plan always runs
      int pick = r.next() % 20;
      p.move = pick < 6 ? FLEE : pick < 9 ? STRAFE_LEFT : pick < 12 ? STRAFE_RIGHT : (int)(r.next() % CR_MOVE);
      p.aim_rank = std::min<int>(r.next() % 4, 2);
      p.perk = i % 7;
      p.jitter = r.next() % 3 == 0 ? 0.25f : 0;
    }
    // A perk shapes the rest of the run: judge segments that may take one over a longer lookahead.
    cur.observe(obs_.data());
    int lookahead = obs_[CR_OFF_SCALARS + 36] > 0 ? 3 * L : L;
    std::vector<Outcome> outs(M);
    long sim = 0;
#pragma omp parallel for schedule(dynamic, 1) reduction(+ : sim)
    for (int i = 0; i < M; ++i) {
      Env &e = *envs_[i];
      float *o = obs_k_[i].data();
      e.copy_from(cur);
      e.observe(o);
      int xp0 = e.experience();
      Rng r{mix(seed ^ mix(segment * 31 + salt * 17 + i + 1))};
      Outcome &out = outs[i];
      out.candidate = i;
      out.died_at = -1;
      int t = 0;
      bool done = false;
      for (; t < K + lookahead && !done; ++t) {
        Action a = act(t < K || i % 2 ? plans[i] : Plan{}, o, r);  // odd candidates keep their plan
        if (t < K) out.segment.push_back(a);
        e.step(a.data(), o, &done);
        if (t == K - 1) out.at_k = e.state_hash();
        if (done) out.died_at = t;
      }
      out.xp = e.experience() - xp0;
      out.health = e.alive() ? e.health() : 0;
      out.crowd = o[CR_OFF_SCALARS + 47] * 32;
      out.energizer = energizer_seen(o);
      sim += t;
    }
    if (simulated) *simulated += sim;
    int best_xp = 0;
    for (const Outcome &o : outs) best_xp = std::max(best_xp, o.xp);
    int best = 0;
    for (int i = 1; i < M; ++i)
      if (value(outs[i], best_xp, energizer) > value(outs[best], best_xp, energizer)) best = i;
    return outs[best];
  }

 private:
  int M;
  std::vector<float> obs_;
  std::vector<std::vector<float>> obs_k_;
  std::vector<std::unique_ptr<Env>> envs_;
};

}  // namespace crimson
