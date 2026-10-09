/* PufferLib 4.0 binding for the Crimsonland env (env/capi.h); one agent per env. */
#define _GNU_SOURCE 1
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "capi.h"
#include "layout.h"

typedef struct {
    float perf, score, episode_return, episode_length, perks, reveal_failed, game_errors, picks_deferred;
    float n; /* runs; must be last */
} Log;

typedef struct {
    Log log;
    float *observations, *actions, *rewards, *terminals;
    int num_agents;
    unsigned int rng;
    CrimsonEnv *env;
} Crimson;

static void take_stats(Crimson *e) {
    CrimsonEnvStats s;
    crimson_env_take_stats(e->env, &s);
    if (s.runs == 0) return;
    e->log.perf += (float)(s.score / 100000.0);
    e->log.score += (float)s.score;
    e->log.episode_return += (float)s.episode_return;
    e->log.episode_length += (float)s.ticks;
    e->log.perks += (float)s.perks;
    e->log.reveal_failed += (float)s.reveal_failed;
    e->log.game_errors += (float)s.game_errors;
    e->log.picks_deferred += (float)s.picks_deferred;
    e->log.n += (float)s.runs;
}

void c_reset(Crimson *e) {
    crimson_env_reset(e->env, e->observations);
    e->rewards[0] = 0;
    e->terminals[0] = 0;
}

void c_step(Crimson *e) {
    static const int sizes[CR_NUM_ATNS] = {CR_MOVE, CR_AIM, CR_FIRE, CR_RELOAD, CR_PERK};
    int a[CR_NUM_ATNS];
    for (int i = 0; i < CR_NUM_ATNS; i++) {
        int v = (int)lrintf(e->actions[i]);
        a[i] = v < 0 ? 0 : v >= sizes[i] ? sizes[i] - 1 : v;
    }
    int done;
    e->rewards[0] = crimson_env_step(e->env, a, e->observations, &done);
    e->terminals[0] = (float)done;
    if (done) take_stats(e);
}

void c_render(Crimson *e) { (void)e; }

void c_close(Crimson *e) {
    crimson_env_free(e->env);
    e->env = NULL;
}

#define OBS_SIZE CR_OBS_SIZE
#define NUM_ATNS CR_NUM_ATNS
#define ACT_SIZES {CR_MOVE, CR_AIM, CR_FIRE, CR_RELOAD, CR_PERK}
#define OBS_TENSOR_T FloatTensor
#define Env Crimson
#include "vecenv.h"

static double option(Dict *kwargs, const char *name, double fallback) {
    DictItem *item = dict_get_unsafe(kwargs, name);
    return item ? item->value : fallback;
}

void my_init(Env *env, Dict *kwargs) {
    /* Ints and floats both arrive as doubles; fractions travel as thousandths, as PufferLib's ini layer
     * keeps [env] values integral. */
    CrimsonEnvConfig c;
    c.mode = (int)option(kwargs, "mode", 1);
    c.repeat = (int)option(kwargs, "action_repeat", 2);
    c.max_ticks = (long)option(kwargs, "max_ticks", 0);
    c.xp_scale = (float)option(kwargs, "xp_scale_milli", 10) / 1000.0f;
    c.death_penalty = (float)option(kwargs, "death_penalty_milli", 1000) / 1000.0f;
    c.alive_reward = (float)option(kwargs, "alive_reward_milli", 0) / 1000.0f;
    uint64_t seed = ((uint64_t)(uint32_t)option(kwargs, "seed", 73) << 32) | env->rng;
    const char *core = getenv("CRIMSON_CORE_SO");
#ifdef CRIMSON_CORE_SO_DEFAULT
    if (!core) core = CRIMSON_CORE_SO_DEFAULT;
#endif
    env->num_agents = 1;
    env->env = crimson_env_new(&c, seed, core);
}

void my_log(Log *log, Dict *out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "perks", log->perks);
    dict_set(out, "reveal_failed", log->reveal_failed);
    dict_set(out, "game_errors", log->game_errors);
    dict_set(out, "picks_deferred", log->picks_deferred);
}
