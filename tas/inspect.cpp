// Replays a TAS run's committed decisions (<out>.actions) and prints, every `every` seconds, the experience and its
// rate, health, and the weapons in hand: inspect <libcrimson_core.so> <run.actions> [every=30]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "env.hpp"
#include "policy.hpp"

using namespace crimson;

int main(int argc, char **argv) {
  if (argc < 3) return fprintf(stderr, "usage: inspect <libcrimson_core.so> <run.actions> [every]\n"), 2;
  set_core_library(argv[1]);
  FILE *f = fopen(argv[2], "rb");
  uint32_t seed;
  if (!f || fread(&seed, sizeof seed, 1, f) != 1) return 1;
  std::vector<Action> acts;
  Action a;
  while (fread(&a, sizeof a, 1, f) == 1) acts.push_back(a);
  fclose(f);
  double every = argc > 3 ? atof(argv[3]) : 30;
  EnvConfig cfg;
  cfg.auto_reset = false;
  std::vector<float> o(CR_OBS_SIZE);
  Env e(cfg, 1);
  e.reset(seed, o.data());
  bool done = false;
  int last_xp = 0, weapon = -1;
  double next = every;
  for (size_t i = 0; i < acts.size() && !done; ++i) {
    e.step(acts[i].data(), o.data(), &done);
    double s = e.ticks() / 60.0;
    int w = (int)o[CR_OFF_IDS];
    if (w != weapon) printf("%7.1f s  weapon %d (alt %d)\n", s, w, (int)o[CR_OFF_IDS + 1]), weapon = w;
    if (s >= next || done) {
      printf("%7.1f s  xp %9d  +%8.0f/min  health %5.1f\n", s, e.experience(), (e.experience() - last_xp) * 60 / every,
             e.alive() ? e.health() : 0.0f);
      last_xp = e.experience(), next += every;
    }
  }
}
