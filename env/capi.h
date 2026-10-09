/* C interface to the env (env/env.hpp), for PufferLib's binding.c. */
#ifndef CRIMSON_ENV_CAPI_H
#define CRIMSON_ENV_CAPI_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CrimsonEnv CrimsonEnv;

typedef struct {
  int mode, repeat;
  long max_ticks;
  float xp_scale, death_penalty, alive_reward;
} CrimsonEnvConfig;

typedef struct {
  double runs, score, ticks, episode_return, perks, reveal_failed, game_errors, picks_deferred;
} CrimsonEnvStats;

/* core_so: the world-swappable core library; NULL keeps the current one ($CRIMSON_CORE_SO by default). */
CrimsonEnv *crimson_env_new(const CrimsonEnvConfig *config, uint64_t rng_seed, const char *core_so);
void crimson_env_free(CrimsonEnv *env);
void crimson_env_reset(CrimsonEnv *env, float *obs);
void crimson_env_reset_seed(CrimsonEnv *env, uint32_t seed, float *obs); /* a run on this game seed */
/* actions: CR_NUM_ATNS choices. Returns the reward; *done is 1 when a run ended (obs then starts the next). */
float crimson_env_step(CrimsonEnv *env, const int *actions, float *obs, int *done);
/* Stats of runs finished since the last call. */
void crimson_env_take_stats(CrimsonEnv *env, CrimsonEnvStats *out);

/* The TAS search (tas/search.hpp) as an expert for DAgger: labels a state with the decision the search would make
 * there. */
typedef struct CrimsonExpert CrimsonExpert;
CrimsonExpert *crimson_expert_new(const CrimsonEnvConfig *config, int candidates);
void crimson_expert_free(CrimsonExpert *expert);
/* Writes the best candidate's first decision (CR_NUM_ATNS ints) for env's state, searching `segment` decisions
 * then `lookahead` more; `salt` varies the candidates. Returns the decision that candidate died at, or -1. */
int crimson_expert_label(CrimsonExpert *expert, CrimsonEnv *env, int segment, int lookahead, long salt,
                         int *action);

#ifdef __cplusplus
}
#endif
#endif
