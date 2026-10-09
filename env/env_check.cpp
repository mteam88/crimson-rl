// Checks and benchmarks the env (env/env.cpp) with a scripted policy that reads its observation:
//  1. observations are finite; prints each section's largest magnitude, for calibrating the scales;
//  2. a recorded run, perk menu and picks included, replays through the WASM verifier snapshot-for-snapshot,
//     and passes the leaderboard's ranked checks (core/ranked_check.mjs) with the score we derived;
//  3. throughput with observations, T threads x W envs (envs reset on the main thread, stepped on workers).
// Usage: env_check <libcrimson_core.so> <reference command> <ranked command> [threads] [envs per thread] [seconds]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "env.hpp"
#include "world.hpp"

using namespace crimson;

// Aims at the nearest creature, walks away from it, fires, opens the perk menu then takes a random choice; now and
// then it picks blind or lets an open menu close unpicked, so picks come with no menu open too.
static void script(const float *obs, uint64_t &rng, int *a) {
  rng = rng * 6364136223846793005ull + 1442695040888963407ull;
  uint32_t r = rng >> 33;
  const float *c = obs + CR_OFF_CREATURES;
  float ang = 0, away = 0;
  if (c[0] > 0) {
    ang = atan2f(c[2], c[1]);
    away = ang + 3.14159265f + ((r & 0xff) / 255.0f - 0.5f) * 1.5f;
  }
  int aim = (int)lroundf(ang / (2 * 3.14159265f) * CR_AIM);
  int move = (int)lroundf(away / (2 * 3.14159265f) * (CR_MOVE - 1));
  a[0] = c[0] > 0 && c[3] < 0.6f ? 1 + ((move % (CR_MOVE - 1)) + (CR_MOVE - 1)) % (CR_MOVE - 1) : (r >> 8) % CR_MOVE;
  a[1] = (aim % CR_AIM + CR_AIM) % CR_AIM;
  a[2] = 1;
  a[3] = (r >> 12) % 97 == 0;
  bool pending = obs[CR_OFF_SCALARS + 36] > 0, revealed = obs[CR_OFF_SCALARS + 37] > 0;
  int pick = 2 + (r >> 16) % 5;
  a[4] = !pending ? 0 : !revealed ? ((r >> 20) % 4 ? 1 : pick) : (r >> 22) % 4 ? pick : 0;
}

static uint64_t fnv(const uint32_t *p, int n) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

static int fails = 0;
static void expect(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  fails += !ok;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: env_check <libcrimson_core.so> <reference command> <ranked command> [threads] [envs] "
                    "[seconds]\n");
    return 2;
  }
  set_core_library(argv[1]);
  int threads = argc > 4 ? atoi(argv[4]) : 4, per = argc > 5 ? atoi(argv[5]) : 16;
  double seconds = argc > 6 ? atof(argv[6]) : 5;

  // 1 and 2: one recorded env, played until a run with perks taken finishes.
  {
    EnvConfig cfg;
    cfg.record = true;
    Env env(cfg, 12345);
    std::vector<float> obs(CR_OBS_SIZE);
    env.reset(obs.data());
    const char *names[] = {"scalars", "ids", "perks", "creatures", "shots", "bonuses", "local", "global"};
    int offs[] = {CR_OFF_SCALARS, CR_OFF_IDS,     CR_OFF_PERKS, CR_OFF_CREATURES,
                  CR_OFF_SHOTS,   CR_OFF_BONUSES, CR_OFF_LOCAL, CR_OFF_GLOBAL, CR_OBS_SIZE};
    float maxabs[8] = {};
    std::vector<float> field_max(CR_SCALARS + CR_CREATURE_F + CR_SHOT_F + CR_BONUS_F);
    bool finite = true;
    uint64_t rng = 7;
    int a[CR_NUM_ATNS];
    long decisions = 0, rerolled = 0, reopened = 0, deferred_all = 0;
    while (true) {
      script(obs.data(), rng, a);
      bool done;
      std::vector<float> ids(obs.begin() + CR_OFF_IDS + 2, obs.begin() + CR_OFF_IDS + 9);
      bool revealed = obs[CR_OFF_SCALARS + 37] > 0;
      double deferred = env.stats.picks_deferred;
      env.step(a, obs.data(), &done);
      // A pick deferred to reopen a menu whose choices were revealed keeps them.
      deferred_all += env.stats.picks_deferred > deferred;
      if (!done && revealed && env.stats.picks_deferred > deferred) {
        ++reopened;
        rerolled += !std::equal(ids.begin(), ids.end(), obs.begin() + CR_OFF_IDS + 2);
      }
      ++decisions;
      for (int s = 0; s < 8; ++s)
        for (int i = offs[s]; i < offs[s + 1]; ++i) {
          finite &= std::isfinite(obs[i]);
          maxabs[s] = std::max(maxabs[s], fabsf(obs[i]));
        }
      for (int i = 0; i < CR_SCALARS; ++i) field_max[i] = std::max(field_max[i], fabsf(obs[CR_OFF_SCALARS + i]));
      for (int k = 0; k < CR_CREATURES; ++k)
        for (int i = 0; i < CR_CREATURE_F; ++i)
          field_max[CR_SCALARS + i] =
              std::max(field_max[CR_SCALARS + i], fabsf(obs[CR_OFF_CREATURES + k * CR_CREATURE_F + i]));
      for (int k = 0; k < CR_SHOTS; ++k)
        for (int i = 0; i < CR_SHOT_F; ++i)
          field_max[CR_SCALARS + CR_CREATURE_F + i] =
              std::max(field_max[CR_SCALARS + CR_CREATURE_F + i], fabsf(obs[CR_OFF_SHOTS + k * CR_SHOT_F + i]));
      for (int k = 0; k < CR_BONUSES; ++k)
        for (int i = 0; i < CR_BONUS_F; ++i)
          field_max[CR_SCALARS + CR_CREATURE_F + CR_SHOT_F + i] =
              std::max(field_max[CR_SCALARS + CR_CREATURE_F + CR_SHOT_F + i],
                       fabsf(obs[CR_OFF_BONUSES + k * CR_BONUS_F + i]));
      if (done && env.stats.perks > 0 && reopened > 0) break;
      if (done) env.stats = EnvStats{};
      if (decisions > 2000000) break;
    }
    printf("run: seed %u, score %d, %.0f ticks, %.0f perks, %.0f picks deferred to an open, %.0f menu opens failed, "
           "%.0f game errors; over every run, %ld picks deferred, %ld reopening revealed choices (%ld rerolled)\n",
           env.seed(), env.last_score(), env.stats.ticks, env.stats.perks, env.stats.picks_deferred,
           env.stats.reveal_failed, env.stats.game_errors, deferred_all, reopened, rerolled);
    for (int s = 0; s < 8; ++s) printf("  %-9s max |x| %.3g\n", names[s], maxabs[s]);
    auto dump = [&](const char *what, int base, int n) {
      printf("  %s field max:", what);
      for (int i = 0; i < n; ++i) printf(" %.2g", field_max[base + i]);
      printf("\n");
    };
    dump("scalars", 0, CR_SCALARS);
    dump("creature", CR_SCALARS, CR_CREATURE_F);
    dump("shot", CR_SCALARS + CR_CREATURE_F, CR_SHOT_F);
    dump("bonus", CR_SCALARS + CR_CREATURE_F + CR_SHOT_F, CR_BONUS_F);
    expect(finite, "observations are finite");
    expect(env.stats.game_errors == 0, "the core accepted every input and command");
    expect(env.stats.perks > 0 && env.stats.reveal_failed == 0, "perk menu opens and picks work");
    expect(deferred_all > 0 && reopened > 0 && rerolled == 0,
           "a pick with no menu open opens it instead, keeping revealed choices");

    // Replay the recorded run: our own world's snapshots against the WASM verifier's.
    const std::vector<uint8_t> &t = env.last_transport();
    char tmpl[] = "/tmp/crimson-env-XXXXXX";
    int fd = mkstemp(tmpl);
    if (write(fd, t.data(), t.size()) != (ssize_t)t.size()) return 1;
    close(fd);
    std::vector<uint64_t> ours;
    {
      Lib lib(argv[1]);
      World *w = lib.create();
      lib.use(w);
      memcpy(reinterpret_cast<void *>(lib.config()), t.data(), sizeof(PortableConfig));
      auto *cfgp = reinterpret_cast<PortableConfig *>(lib.config());
      lib.init(cfgp->seed, cfgp->mode, cfgp->major, cfgp->minor);
      auto snap = [&] { ours.push_back(fnv(reinterpret_cast<const uint32_t *>(lib.output()), lib.snapshot())); };
      snap();
      for (size_t at = sizeof(PortableConfig); at < t.size();) {
        memcpy(reinterpret_cast<void *>(lib.input()), &t[at], sizeof(PortableInput));
        uint32_t count;
        memcpy(&count, &t[at + sizeof(PortableInput)], 4);
        at += sizeof(PortableInput) + 4;
        memcpy(reinterpret_cast<void *>(lib.commands()), &t[at], count * sizeof(PortableCommand));
        at += count * sizeof(PortableCommand);
        if (!lib.step_many(count)) break;
        snap();
      }
      lib.destroy(w);
    }
    std::string cmd = std::string(argv[2]) + " < " + tmpl;
    FILE *p = popen(cmd.c_str(), "r");
    std::vector<uint64_t> theirs;
    std::vector<uint32_t> buf;
    uint32_t n;
    while (fread(&n, 4, 1, p) == 1) {
      buf.resize(n);
      if (fread(buf.data(), 4, n, p) != n) break;
      theirs.push_back(fnv(buf.data(), n));
    }
    pclose(p);
    std::string ranked = std::string(argv[3]) + " < " + tmpl;
    p = popen(ranked.c_str(), "r");
    char verdict[512] = {};
    size_t got = fread(verdict, 1, sizeof verdict - 1, p);
    int status = pclose(p);
    verdict[got] = 0;
    printf("ranked check: %s", verdict);
    char want[64];
    snprintf(want, sizeof want, "\"experience\":%d,", env.last_score());
    expect(status == 0 && strstr(verdict, want), "the recorded run ranks, with the score we derived");
    unlink(tmpl);
    char msg[160];
    snprintf(msg, sizeof msg, "a recorded run replays through the WASM verifier (%zu snapshots, ours %zu)",
             theirs.size(), ours.size());
    expect(ours == theirs && ours.size() > 1, msg);
  }

  // 3. Throughput.
  {
    EnvConfig cfg;
    std::vector<std::unique_ptr<Env>> envs;
    std::vector<std::vector<float>> obs(threads * per, std::vector<float>(CR_OBS_SIZE));
    for (int i = 0; i < threads * per; ++i) {
      envs.push_back(std::make_unique<Env>(cfg, 1000 + i));
      envs.back()->reset(obs[i].data());
    }
    std::atomic<bool> stop{false};
    std::atomic<long> decisions{0};
    std::vector<std::thread> ts;
    for (int k = 0; k < threads; ++k)
      ts.emplace_back([&, k] {
        uint64_t rng = k + 1;
        long n = 0;
        int a[CR_NUM_ATNS];
        while (!stop.load(std::memory_order_relaxed)) {
          for (int i = k * per; i < (k + 1) * per; ++i) {
            script(obs[i].data(), rng, a);
            bool done;
            envs[i]->step(a, obs[i].data(), &done);
          }
          n += per;
        }
        decisions += n;
      });
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto &t : ts) t.join();
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double runs = 0, score = 0;
    for (auto &e : envs) runs += e->stats.runs, score += e->stats.score;
    printf("throughput: %d threads x %d envs: %.0f decisions/s (%.0f ticks/s, %.0f per thread); %.0f runs, mean "
           "score %.0f; obs %d floats\n",
           threads, per, decisions / el, decisions / el * cfg.repeat, decisions / el * cfg.repeat / threads, runs,
           runs ? score / runs : 0, CR_OBS_SIZE);
    envs.clear();  // destroys worlds on the main thread, which ran none of them last
  }
  return fails ? 1 : 0;
}
