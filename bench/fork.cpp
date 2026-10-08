// Save/restore probe: copy the executable's .data+.bss, branch, restore, replay, and check the runs agree.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "api.h"

extern "C" char __data_start[], _end[];
extern "C" int core_main(int, char **);

static void act(long t, float px, float py, uint32_t salt) {
  auto *in = reinterpret_cast<PortableInput *>(portable_input());
  float s = t / 60.0f + salt;
  in->move_x = cosf(s * 0.7f);
  in->move_y = sinf(s * 0.7f);
  in->aim_x = px + 100 * cosf(s * 3.0f);
  in->aim_y = py + 100 * sinf(s * 3.0f);
  in->flags = 0x100 | (3u << 9) | 0x1000 | 1;
}

static long run(long n, uint32_t salt, long t0) {
  auto *probe = reinterpret_cast<PortableProbe *>(portable_probe());
  for (long i = 0; i < n; ++i) {
    act(t0 + i, portable_player_x(), portable_player_y(), salt);
    if (!portable_step(0, 0)) break;
  }
  portable_probe();
  return (long)probe->experience * 1000003L + (long)(probe->health * 1000) + probe->kills;
}

int main() {
  auto *cfg = reinterpret_cast<PortableConfig *>(portable_config());
  memset(cfg, 0, sizeof(*cfg));
  cfg->seed = 7; cfg->mode = 1; cfg->detail = 5;
  portable_init(7, 1, 1, 1);
  run(600, 0, 0);
  size_t bytes = _end - __data_start;
  std::vector<char> save(__data_start, _end);
  long a = run(600, 1, 600);
  memcpy(__data_start, save.data(), bytes);
  long b = run(600, 2, 600);
  memcpy(__data_start, save.data(), bytes);
  long c = run(600, 1, 600);
  printf("state bytes copied %zu | branch A %ld, branch B %ld, A again %ld -> %s\n", bytes, a, b, c,
         a == c && a != b ? "restore is exact" : "MISMATCH");
  auto t = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) memcpy(__data_start, save.data(), bytes);
  printf("restore cost %.1f us\n", std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count() / 2000);
}
