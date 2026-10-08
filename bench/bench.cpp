// Throughput probe for crimson-core: one Survival run after another, a dumb circling bot that fires at a rotating point.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "api.h"

extern "C" int core_main(int, char **);

int main(int argc, char **argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 10.0;
  int mode = argc > 2 ? atoi(argv[2]) : 1;
  auto *cfg = reinterpret_cast<PortableConfig *>(portable_config());
  auto *in = reinterpret_cast<PortableInput *>(portable_input());
  auto *probe = reinterpret_cast<PortableProbe *>(portable_probe());
  const uint32_t flags = 0x100 | (3u << 9) | 0x1000 | (0u << 13);  // pad move, mouse aim
  long ticks = 0, runs = 0, total_xp = 0, run_ticks = 0, max_run = 0;
  uint32_t seed = 1;
  auto start = std::chrono::steady_clock::now();
  auto init = [&] {
    memset(cfg, 0, sizeof(*cfg));
    cfg->seed = seed++;
    cfg->mode = mode;
    cfg->detail = 5;
    cfg->unlock = cfg->unlock_full = 1;
    if (!portable_init(cfg->seed, mode, 1, 1)) { fprintf(stderr, "init failed\n"); exit(1); }
    run_ticks = 0;
  };
  init();
  for (;;) {
    float t = run_ticks / 60.0f;
    float px = portable_player_x(), py = portable_player_y();
    in->move_x = cosf(t * 0.7f);
    in->move_y = sinf(t * 0.7f);
    in->aim_x = px + 100 * cosf(t * 3.0f);
    in->aim_y = py + 100 * sinf(t * 3.0f);
    in->flags = flags | 1 | ((run_ticks % 137 == 0) ? 65536 : 0);
    int ok = portable_step(0, 0);
    ++ticks;
    ++run_ticks;
    if (!ok || run_ticks > 60 * 60 * 30) {
      portable_probe();
      total_xp += probe->experience;
      if (run_ticks > max_run) max_run = run_ticks;
      ++runs;
      if ((ticks & 1023) == 0 || true) {
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (el > seconds) break;
      }
      init();
    }
    if ((ticks & 4095) == 0) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (el > seconds) break;
    }
  }
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  printf("ticks %ld in %.2fs = %.0f ticks/s (%.1fx realtime) | runs %ld, mean xp %.0f, longest run %.1fs game time\n",
         ticks, el, ticks / el, ticks / el / 60, runs, runs ? (double)total_xp / runs : 0.0, max_run / 60.0);
}
