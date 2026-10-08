// First divergence between a world run and upstream's core binary on the same inputs.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "world.hpp"
using namespace crimson;
int main(int argc, char **argv) {
  Lib lib(argv[1]);
  World *w = lib.create();
  lib.use(w);
  auto *cfg = reinterpret_cast<PortableConfig *>(lib.config());
  memset(cfg, 0, sizeof *cfg); cfg->seed = 7; cfg->mode = 1; cfg->detail = 5;
  PortableConfig keep = *cfg;
  lib.init(7, 1, 1, 1);
  std::vector<std::vector<uint32_t>> ours;
  auto snap = [&] { int n = lib.snapshot(); auto *o = (uint32_t *)lib.output(); ours.emplace_back(o, o + n); };
  snap();
  std::vector<PortableInput> inputs;
  for (long t = 0; t < 6000; ++t) {
    auto *in = reinterpret_cast<PortableInput *>(lib.input());
    float s = t / 60.0f + 7;
    in->move_x = cosf(s * 0.7f); in->move_y = sinf(s * 0.7f);
    in->aim_x = lib.player_x() + 100 * cosf(s * 3); in->aim_y = lib.player_y() + 100 * sinf(s * 3);
    in->flags = 0x100 | (3u << 9) | 0x1000 | 1 | (t % 137 == 0 ? 65536 : 0);
    PortableInput rec = *in;
    if (!lib.step(0, 0)) break;
    inputs.push_back(rec);
    snap();
  }
  FILE *f = fopen("/tmp/diverge.rsi", "wb");
  fwrite(&keep, sizeof keep, 1, f);
  uint32_t zero = 0;
  for (auto &in : inputs) { fwrite(&in, sizeof in, 1, f); fwrite(&zero, 4, 1, f); }
  fclose(f);
  FILE *p = popen((std::string(argv[2]) + " < /tmp/diverge.rsi").c_str(), "r");
  uint32_t n; size_t i = 0;
  std::vector<uint32_t> buf;
  while (fread(&n, 4, 1, p) == 1) {
    buf.resize(n);
    fread(buf.data(), 4, n, p);
    if (i >= ours.size()) { printf("theirs longer at %zu\n", i); return 1; }
    if (buf != ours[i]) {
      printf("first divergence at snapshot %zu (sizes ours %zu theirs %u)\n", i, ours[i].size(), n);
      int shown = 0;
      for (size_t k = 0; k < std::min<size_t>(n, ours[i].size()) && shown < 15; ++k)
        if (buf[k] != ours[i][k]) { printf("  field %zu ours %08x theirs %08x\n", k, ours[i][k], buf[k]); ++shown; }
      return 1;
    }
    ++i;
  }
  printf("compared %zu snapshots, ours %zu: %s\n", i, ours.size(), i == ours.size() ? "identical" : "LENGTH DIFFERS");
}
