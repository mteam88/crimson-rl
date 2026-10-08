#include "capi.h"

#include "env.hpp"
#include "search.hpp"

using crimson::Env;

struct CrimsonEnv {
  Env env;
  CrimsonEnv(const crimson::EnvConfig &c, uint64_t seed) : env(c, seed) {}
};

static crimson::EnvConfig env_config(const CrimsonEnvConfig *config) {
  crimson::EnvConfig c;
  c.mode = config->mode;
  c.repeat = config->repeat;
  c.max_ticks = config->max_ticks;
  c.xp_scale = config->xp_scale;
  c.death_penalty = config->death_penalty;
  c.alive_reward = config->alive_reward;
  return c;
}

extern "C" CrimsonEnv *crimson_env_new(const CrimsonEnvConfig *config, uint64_t rng_seed, const char *core_so) {
  if (core_so) crimson::set_core_library(core_so);
  return new CrimsonEnv(env_config(config), rng_seed);
}

extern "C" void crimson_env_free(CrimsonEnv *env) { delete env; }

extern "C" void crimson_env_reset(CrimsonEnv *env, float *obs) { env->env.reset(obs); }

extern "C" void crimson_env_reset_seed(CrimsonEnv *env, uint32_t seed, float *obs) { env->env.reset(seed, obs); }

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

struct CrimsonExpert {
  crimson::Search search;
  CrimsonExpert(const crimson::EnvConfig &c, int candidates) : search(c, candidates) {}
};

extern "C" CrimsonExpert *crimson_expert_new(const CrimsonEnvConfig *config, int candidates) {
  crimson::EnvConfig c = env_config(config);
  c.auto_reset = false;
  return new CrimsonExpert(c, candidates);
}

extern "C" void crimson_expert_free(CrimsonExpert *expert) { delete expert; }

extern "C" int crimson_expert_label(CrimsonExpert *expert, CrimsonEnv *env, int segment, int lookahead, long salt,
                                    int *action) {
  crimson::Outcome b = expert->search.best(env->env, env->env.seed(), env->env.ticks(), (int)salt, segment,
                                           lookahead, nullptr);
  for (int i = 0; i < CR_NUM_ATNS; ++i) action[i] = b.segment[0][i];
  return b.died_at;
}
