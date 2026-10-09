// How much experience each weapon earns late in a run: replays a TAS run's decisions to `at` seconds, then for every
// weapon plays the scripted controller (no perks) for `secs` seconds from there with that weapon in hand, over `n`
// rollouts: weapons <libcrimson_core.so> <run.actions> <at> [secs=10] [n=16]
#include <omp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "env.hpp"
#include "policy.hpp"

using namespace crimson;

int main(int argc, char **argv) {
  if (argc < 4) return fprintf(stderr, "usage: weapons <libcrimson_core.so> <run.actions> <at> [secs] [n]\n"), 2;
  set_core_library(argv[1]);
  FILE *f = fopen(argv[2], "rb");
  uint32_t seed;
  if (!f || fread(&seed, sizeof seed, 1, f) != 1) return 1;
  std::vector<Action> acts;
  Action a;
  while (fread(&a, sizeof a, 1, f) == 1) acts.push_back(a);
  fclose(f);
  double at = atof(argv[3]), secs = argc > 4 ? atof(argv[4]) : 10;
  int n = argc > 5 ? atoi(argv[5]) : 16;
  EnvConfig cfg;
  cfg.auto_reset = false;
  std::vector<float> o(CR_OBS_SIZE);
  Env base(cfg, 1);
  base.reset(seed, o.data());
  bool done = false;
  for (size_t i = 0; i < acts.size() && !done && base.ticks() < at * 60; ++i) base.step(acts[i].data(), o.data(), &done);
  printf("at %.1f s: xp %d, health %.1f\n", base.ticks() / 60.0, base.experience(), base.health());
  const int W = 34;
  std::vector<double> gain(W * n), died(W * n);
#pragma omp parallel
  {
    Env e(cfg, 2 + omp_get_thread_num());
    std::vector<float> ob(CR_OBS_SIZE);
#pragma omp for schedule(dynamic)
    for (int k = 0; k < (W - 1) * n; ++k) {
      int w = 1 + k / n, r = k % n;
      e.copy_from(base);
      e.debug_set_weapon(w);
      e.observe(ob.data());
      Plan plan;
      plan.keep = 1 << 20;
      Rng rng{mix(r)};
      bool d = false;
      long end = e.ticks() + (long)(secs * 60);
      while (!d && e.ticks() < end) {
        Action x = act(plan, ob.data(), rng);
        e.step(x.data(), ob.data(), &d);
      }
      gain[w * n + r] = e.experience() - base.experience();
      died[w * n + r] = !e.alive();
    }
  }
  std::vector<int> order;
  for (int w = 1; w < W; ++w) order.push_back(w);
  auto mean = [&](int w, std::vector<double> &v) {
    double s = 0;
    for (int r = 0; r < n; ++r) s += v[w * n + r];
    return s / n;
  };
  std::sort(order.begin(), order.end(), [&](int x, int y) { return mean(x, gain) > mean(y, gain); });
  for (int w : order) printf("weapon %2d  xp/min %10.0f  died %3.0f%%\n", w, mean(w, gain) * 60 / secs, 100 * mean(w, died));
}
