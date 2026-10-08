// Which bytes of .data+.bss ever change across runs? Those are the only ones a world switch must copy.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "api.h"
extern "C" char __data_start[], _end[];
extern "C" int core_main(int, char **);
int main() {
  size_t n = _end - __data_start;
  auto *cfg = reinterpret_cast<PortableConfig *>(portable_config());
  auto *in = reinterpret_cast<PortableInput *>(portable_input());
  char *out_lo = reinterpret_cast<char *>(portable_output());
  std::vector<char> base(__data_start, _end);   // before any init
  std::vector<unsigned char> dirty(n, 0);
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->seed = seed; cfg->mode = 1 + seed % 2; cfg->detail = 5;
    portable_init(seed, cfg->mode, 1, 1);
    for (long t = 0; t < 20000; ++t) {
      float s = t / 60.0f + seed;
      in->move_x = cosf(s * 0.7f); in->move_y = sinf(s * 0.7f);
      in->aim_x = portable_player_x() + 100 * cosf(s * 3); in->aim_y = portable_player_y() + 100 * sinf(s * 3);
      in->flags = 0x100 | (3u << 9) | 0x1000 | 1 | ((t % 137 == 0) ? 65536 : 0);
      if (!portable_step(0, 0)) break;
      if (t % 97 == 0) for (size_t i = 0; i < n; ++i) dirty[i] |= __data_start[i] != base[i];
    }
    for (size_t i = 0; i < n; ++i) dirty[i] |= __data_start[i] != base[i];
  }
  size_t out_off = out_lo - __data_start, total = 0, ranges = 0, pages = 0;
  for (size_t i = 0; i < n; ++i) if (dirty[i] && !(i >= out_off && i < out_off + (1 << 20))) ++total;
  for (size_t p = 0; p < n; p += 4096) { bool d = false; for (size_t i = p; i < p + 4096 && i < n; ++i) if (dirty[i] && !(i >= out_off && i < out_off + (1 << 20))) d = true; pages += d; }
  for (size_t i = 0; i < n;) { if (dirty[i]) { size_t j = i; while (j < n && (dirty[j] || (j + 64 < n && dirty[j+64]))) ++j; ++ranges; i = j; } else ++i; }
  printf("region %zu bytes; changed bytes (excl output) %zu; dirty 4K pages %zu (%zu KB); ranges %zu\n", n, total, pages, pages * 4, ranges);
}
