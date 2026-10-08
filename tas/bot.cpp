// The scripted controller (tas/policy.hpp) playing live, no search or rollback: a baseline for the RL policy.
// Plays seeds first..first+runs-1 in parallel and prints each run's experience and length.
// Usage: bot <libcrimson_core.so> <first seed> [runs=32] [record best to transport]
#include <omp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "env.hpp"
#include "policy.hpp"

using namespace crimson;

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: bot <libcrimson_core.so> <first seed> [runs] [out transport]\n");
    return 2;
  }
  set_core_library(argv[1]);
  uint32_t first = strtoul(argv[2], nullptr, 0);
  int runs = argc > 3 ? atoi(argv[3]) : 32;
  const char *out = argc > 4 ? argv[4] : nullptr;

  struct Run {
    int xp = 0;
    long ticks = 0;
    std::vector<uint8_t> transport;
  };
  std::vector<Run> results(runs);
#pragma omp parallel for schedule(dynamic, 1)
  for (int i = 0; i < runs; ++i) {
    EnvConfig cfg;
    cfg.auto_reset = false;
    cfg.record = out != nullptr;
    Env env(cfg, 1);
    std::vector<float> obs(CR_OBS_SIZE);
    env.reset(first + i, obs.data());
    Rng rng{mix(first + i)};
    Plan plan;
    bool done = false;
    while (!done) {
      Action a = act(plan, obs.data(), rng);
      env.step(a.data(), obs.data(), &done);
    }
    results[i] = {env.last_score(), env.ticks(), out ? env.last_transport() : std::vector<uint8_t>{}};
    printf("seed %u: experience %d, %.2f minutes\n", first + i, results[i].xp, results[i].ticks / 3600.0);
    fflush(stdout);
  }
  std::vector<int> xp;
  for (auto &r : results) xp.push_back(r.xp);
  std::sort(xp.begin(), xp.end());
  double mean = 0;
  for (int x : xp) mean += x;
  printf("%d runs: mean %.0f, median %d, best %d, worst %d\n", runs, mean / runs, xp[runs / 2], xp.back(), xp[0]);
  if (out) {
    auto best = std::max_element(results.begin(), results.end(), [](auto &a, auto &b) { return a.xp < b.xp; });
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(best->transport.data(), 1, best->transport.size(), f) != best->transport.size()) return 1;
    fclose(f);
  }
  return 0;
}
