// A tool-assisted Survival run on a chosen seed: a search over the inputs with save/restore. From the committed
// state it plays candidate plans in parallel, each for one segment and then a lookahead under a default plan, and
// commits the segment of the best (survival first, then experience and health).
// When every candidate dies inside
// its segment, it backs up some segments (further on each failure) and tries new candidates there.
// The committed actions then replay in a fresh recording env, which writes the run as a transport (check it with
// core/ranked_check.mjs).
// It also writes the committed decisions to <out transport>.actions, for puffer/bc.py.
// Usage: tas <libcrimson_core.so> <seed> <out transport> [candidates] [segment] [lookahead] [max minutes]
#include <omp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "env.hpp"
#include "policy.hpp"

using namespace crimson;

namespace {

struct Outcome {
  int died_at;  // decision the run ended in, or -1
  int xp;       // experience gained
  float health;
  float crowd;  // creatures within 256 units at the end
  std::vector<Action> segment;
  uint64_t at_k = 0;  // the world's hash after the segment, to check the commit reproduces it
};

// Survival first (later death is better); then experience and health, less crowding. A full bar is worth 2000
// experience, plus a quarter of the best candidate's gain so it still counts late, when experience comes in floods;
// each creature left within 256 units costs 2% of a bar.
double value(const Outcome &o, int best_xp) {
  if (o.died_at >= 0) return -1e9 + o.died_at;
  double bar = 2000 + 0.25 * best_xp;
  return o.xp + (o.health / 100 - 0.02 * o.crowd) * bar;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: tas <libcrimson_core.so> <seed> <out transport> [candidates] [segment] [lookahead] "
                    "[max minutes]\n");
    return 2;
  }
  set_core_library(argv[1]);
  uint32_t seed = (uint32_t)strtoul(argv[2], nullptr, 0);
  const char *out = argv[3];
  int M = argc > 4 ? atoi(argv[4]) : 64, K = argc > 5 ? atoi(argv[5]) : 8, L = argc > 6 ? atoi(argv[6]) : 120;
  double max_minutes = argc > 7 ? atof(argv[7]) : 0;

  EnvConfig cfg;
  cfg.auto_reset = false;
  std::vector<float> obs(CR_OBS_SIZE);
  Env cur(cfg, 1);
  cur.reset(seed, obs.data());
  std::vector<std::unique_ptr<Env>> envs;
  std::vector<std::vector<float>> obs_k(M, std::vector<float>(CR_OBS_SIZE));
  for (int i = 0; i < M; ++i) envs.push_back(std::make_unique<Env>(cfg, 2 + i));

  // Segment-start checkpoints, for backing up.
  constexpr int R = 64;
  std::vector<std::unique_ptr<Env>> saves;
  std::vector<size_t> save_len(R);
  for (int i = 0; i < R; ++i) saves.push_back(std::make_unique<Env>(cfg, 100 + i));

  std::vector<Action> committed;
  long segment = 0, frontier = 0, simulated = 0;
  long oldest = 0;  // the earliest segment whose checkpoint the ring still holds
  int fails = 0, salt = 0;
  auto t0 = std::chrono::steady_clock::now();
  bool over = false;
  while (!over) {
    saves[segment % R]->copy_from(cur);
    save_len[segment % R] = committed.size();
    oldest = std::max(oldest, segment - R + 1);

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
    cur.observe(obs.data());
    int lookahead = obs[CR_OFF_SCALARS + 36] > 0 ? 3 * L : L;
    std::vector<Outcome> outs(M);
#pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < M; ++i) {
      Env &e = *envs[i];
      float *o = obs_k[i].data();
      e.copy_from(cur);
      e.observe(o);
      int xp0 = e.experience();
      Rng r{mix(seed ^ mix(segment * 31 + salt * 17 + i + 1))};
      Outcome &out = outs[i];
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
#pragma omp atomic
      simulated += t;
    }
    int best_xp = 0;
    for (const Outcome &o : outs) best_xp = std::max(best_xp, o.xp);
    int best = 0;
    for (int i = 1; i < M; ++i)
      if (value(outs[i], best_xp) > value(outs[best], best_xp)) best = i;
    const Outcome &b = outs[best];

    if (b.died_at >= 0 && b.died_at < K && fails < 40) {
      // Every candidate dies within the segment: back up further every other failure, with new candidates.
      ++fails;
      ++salt;
      long back = std::min<long>(1L << std::min((fails + 1) / 2, 6), segment - oldest);
      segment -= back;
      cur.copy_from(*saves[segment % R]);
      committed.resize(save_len[segment % R]);
      continue;
    }
    for (const Action &a : b.segment) {
      bool done;
      cur.step(a.data(), obs.data(), &done);
      committed.push_back(a);
      if (done) {
        over = true;
        break;
      }
    }
    if (!over && cur.state_hash() != b.at_k) {
      fprintf(stderr, "segment %ld: the commit diverged from its candidate (%d)\n", segment, best);
      return 1;
    }
    ++segment;
    if (segment > frontier) frontier = segment, fails = 0;
    double minutes = cur.ticks() / 3600.0;
    if (max_minutes > 0 && minutes >= max_minutes) over = true;
    if (segment % 50 == 0 || over) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("%6.2f min  xp %7d  health %5.1f  segment %ld  fails %d  salt %d  %.0f sim ticks/s  %.1fx realtime\n",
             minutes, cur.experience(), cur.alive() ? cur.health() : 0.0f, segment, fails, salt,
             simulated * cfg.repeat / el, cur.ticks() / 60.0 / el);
      fflush(stdout);
    }
  }

  // The committed actions must reproduce the state the search reached.
  {
    Env check(cfg, 1);
    check.reset(seed, obs.data());
    bool done = false;
    for (size_t i = 0; i < committed.size() && !done; ++i) check.step(committed[i].data(), obs.data(), &done);
    if (check.state_hash() != cur.state_hash()) {
      fprintf(stderr, "the committed actions do not reproduce the searched run\n");
      return 1;
    }
  }

  // The decisions too, for training a policy on the run, unfinished or not: <out>.actions holds the seed (u32),
  // then CR_NUM_ATNS int32 per decision. Replaying them in an Env regenerates the observations.
  std::string actions = std::string(out) + ".actions";
  FILE *fa = fopen(actions.c_str(), "wb");
  if (!fa || fwrite(&seed, sizeof seed, 1, fa) != 1 ||
      fwrite(committed.data(), sizeof(Action), committed.size(), fa) != committed.size())
    return 1;
  fclose(fa);

  // Replay the committed actions in a recording env: the transport, and the run played out to its end.
  EnvConfig rcfg = cfg;
  rcfg.record = true;
  Env rec(rcfg, 1);
  rec.reset(seed, obs.data());
  bool done = false;
  for (size_t i = 0; i < committed.size() && !done; ++i) rec.step(committed[i].data(), obs.data(), &done);
  if (!done) {
    fprintf(stderr, "stopped at the time limit: the run is unfinished and will not rank\n");
    return 1;
  }
  const std::vector<uint8_t> &t = rec.last_transport();
  FILE *f = fopen(out, "wb");
  if (!f || fwrite(t.data(), 1, t.size(), f) != t.size()) return 1;
  fclose(f);
  printf("seed %u: experience %d (search saw %d), %.2f minutes; wrote %s\n", seed, rec.last_score(),
         cur.experience(), rec.ticks() / 3600.0, out);
  return 0;
}
