// Throughput with many worlds: T threads, each with its own Lib and W worlds stepped round-robin
// (the switch cost an RL env pool pays). Dumb circling bot; a dead world restarts on a new seed.
// Usage: worlds_bench <libcrimson_core.so> <threads> <worlds per thread> <seconds> [repeat]
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "world.hpp"

using namespace crimson;

int main(int argc, char **argv) {
  const char *so = argv[1];
  int threads = atoi(argv[2]), per = atoi(argv[3]);
  double seconds = atof(argv[4]);
  int repeat = argc > 5 ? atoi(argv[5]) : 1;  // ticks per world before switching (action repeat)
  std::atomic<long> total{0}, deaths{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int k = 0; k < threads; ++k)
    ts.emplace_back([&, k] {
      Lib lib(so);
      std::vector<World *> ws;
      std::vector<long> age(per, 0);
      uint32_t seed = 1000 * k + 1;
      auto begin = [&](int i) {
        lib.use(ws[i]);
        auto *cfg = reinterpret_cast<PortableConfig *>(lib.config());
        memset(cfg, 0, sizeof *cfg);
        cfg->seed = seed;
        cfg->mode = 1;
        cfg->detail = 5;
        lib.init(seed++, 1, 1, 1);
        age[i] = 0;
      };
      for (int i = 0; i < per; ++i) {
        ws.push_back(lib.create());
        begin(i);
      }
      long n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < per; ++i) {
          lib.use(ws[i]);
          auto *in = reinterpret_cast<PortableInput *>(lib.input());
          for (int r = 0; r < repeat; ++r) {
            float s = age[i] / 60.0f + i;
            in->move_x = cosf(s * 0.7f);
            in->move_y = sinf(s * 0.7f);
            in->aim_x = lib.player_x() + 100 * cosf(s * 3);
            in->aim_y = lib.player_y() + 100 * sinf(s * 3);
            in->flags = 0x100 | (3u << 9) | 0x1000 | 1 | (age[i] % 137 == 0 ? 65536 : 0);
            ++n;
            ++age[i];
            if (!lib.step(0, 0)) {
              deaths++;
              begin(i);
              break;
            }
          }
        }
      }
      total += n;
      for (World *w : ws) lib.destroy(w);
    });
  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  stop = true;
  for (auto &t : ts) t.join();
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("threads %d x worlds %d, repeat %d: %.0f ticks/s total (%.0f per thread), %ld runs ended\n", threads, per,
         repeat, total / el, total / el / threads, deaths.load());
}
