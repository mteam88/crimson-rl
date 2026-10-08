#include "capi.h"

#include "env.hpp"

using crimson::Env;

struct CrimsonEnv {
  Env env;
  CrimsonEnv(const crimson::EnvConfig &c, uint64_t seed) : env(c, seed) {}
};

extern "C" CrimsonEnv *crimson_env_new(const CrimsonEnvConfig *config, uint64_t rng_seed, const char *core_so) {
  if (core_so) crimson::set_core_library(core_so);
  crimson::EnvConfig c;
  c.mode = config->mode;
  c.repeat = config->repeat;
  c.max_ticks = config->max_ticks;
  c.xp_scale = config->xp_scale;
  c.death_penalty = config->death_penalty;
  c.alive_reward = config->alive_reward;
  return new CrimsonEnv(c, rng_seed);
}

extern "C" void crimson_env_free(CrimsonEnv *env) { delete env; }

extern "C" void crimson_env_reset(CrimsonEnv *env, float *obs) { env->env.reset(obs); }

extern "C" float crimson_env_step(CrimsonEnv *env, const int *actions, float *obs, int *done) {
  bool d;
  float r = env->env.step(actions, obs, &d);
  *done = d;
  return r;
}

extern "C" void crimson_env_take_stats(CrimsonEnv *env, CrimsonEnvStats *out) {
  const crimson::EnvStats &s = env->env.stats;
  *out = {s.runs, s.score, s.ticks, s.episode_return, s.perks, s.reveal_failed, s.game_errors};
  env->env.stats = {};
}
