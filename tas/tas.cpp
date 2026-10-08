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
#include "search.hpp"

using namespace crimson;

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
  Search search(cfg, M);

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

    const Outcome b = search.best(cur, seed, segment, salt, K, L, &simulated);

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
      fprintf(stderr, "segment %ld: the commit diverged from its candidate (%d)\n", segment, b.candidate);
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
