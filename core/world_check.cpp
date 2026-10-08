// Exactness checks for the world library (core/world.cpp):
//  1. a bot run through a world matches upstream's WASM verifier core snapshot-for-snapshot (upstream's
//     native `core` binary misreads a misaligned global with movaps under clang 22, so it isn't the reference);
//  2. worlds stepped interleaved in one Lib match the same runs stepped alone;
//  3. Libs on separate threads don't disturb each other;
//  4. copy() restores a world exactly (search save/restore).
// Usage: world_check <libcrimson_core.so> <reference command: reads a transport on stdin, writes snapshots>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "world.hpp"

using namespace crimson;

struct Run {
  uint32_t seed;
  int mode;
  long ticks;
};

static uint64_t fnv(const uint32_t *p, int n) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

static void begin(Lib &lib, World *w, const Run &r) {
  lib.use(w);
  auto *cfg = reinterpret_cast<PortableConfig *>(lib.config());
  memset(cfg, 0, sizeof *cfg);
  cfg->seed = r.seed;
  cfg->mode = r.mode;
  cfg->detail = 5;
  if (!lib.init(r.seed, r.mode, 1, 1)) {
    fprintf(stderr, "init failed\n");
    exit(1);
  }
}

// One tick of a state-dependent bot: circles, aims around itself, fires, reloads now and then.
static bool tick(Lib &lib, World *w, long t, uint32_t salt, PortableInput *record) {
  lib.use(w);
  auto *in = reinterpret_cast<PortableInput *>(lib.input());
  float s = t / 60.0f + salt;
  in->move_x = cosf(s * 0.7f);
  in->move_y = sinf(s * 0.7f);
  in->aim_x = lib.player_x() + 100 * cosf(s * 3);
  in->aim_y = lib.player_y() + 100 * sinf(s * 3);
  in->flags = 0x100 | (3u << 9) | 0x1000 | 1 | (t % 137 == 0 ? 65536 : 0);
  if (record) *record = *in;
  return lib.step(0, 0);
}

static uint64_t snap(Lib &lib, World *w) {
  lib.use(w);
  int n = lib.snapshot();
  return fnv(reinterpret_cast<const uint32_t *>(lib.output()), n);
}

static std::vector<uint64_t> solo(Lib &lib, const Run &r, std::vector<PortableInput> *inputs) {
  World *w = lib.create();
  begin(lib, w, r);
  std::vector<uint64_t> h{snap(lib, w)};
  for (long t = 0; t < r.ticks; ++t) {
    PortableInput in;
    if (!tick(lib, w, t, r.seed, &in)) break;
    if (inputs) inputs->push_back(in);
    h.push_back(snap(lib, w));
  }
  lib.destroy(w);
  return h;
}

static int fails = 0;
static void expect(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  fails += !ok;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: world_check <libcrimson_core.so> <reference command>\n");
    return 2;
  }
  std::string so = argv[1];
  Lib lib(so);
  std::vector<Run> runs = {{7, 1, 6000}, {8, 2, 6000}, {9, 1, 6000}};

  // 1. Against upstream's native core, fed the same inputs over its stdin transport.
  {
    std::vector<PortableInput> inputs;
    std::vector<uint64_t> ours = solo(lib, runs[0], &inputs);
    char tmpl[] = "/tmp/crimson-transport-XXXXXX";
    int fd = mkstemp(tmpl);
    FILE *f = fdopen(fd, "wb");
    PortableConfig cfg{};
    cfg.seed = runs[0].seed;
    cfg.mode = runs[0].mode;
    cfg.detail = 5;
    fwrite(&cfg, sizeof cfg, 1, f);
    uint32_t zero = 0;
    for (auto &in : inputs) {
      fwrite(&in, sizeof in, 1, f);
      fwrite(&zero, 4, 1, f);
    }
    fclose(f);
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
    unlink(tmpl);
    char msg[160];
    snprintf(msg, sizeof msg, "world matches the WASM verifier on every snapshot (%zu ticks, %zu inputs)", ours.size(),
             inputs.size());
    expect(ours == theirs, msg);
  }

  // 2. Interleaved worlds in one Lib.
  {
    std::vector<std::vector<uint64_t>> ref;
    for (auto &r : runs) ref.push_back(solo(lib, r, nullptr));
    std::vector<World *> ws;
    std::vector<std::vector<uint64_t>> got(runs.size());
    std::vector<bool> live(runs.size(), true);
    for (size_t i = 0; i < runs.size(); ++i) {
      ws.push_back(lib.create());
      begin(lib, ws[i], runs[i]);
      got[i].push_back(snap(lib, ws[i]));
    }
    for (long t = 0; t < 6000; ++t)
      for (size_t i = 0; i < runs.size(); ++i)
        if (live[i]) {
          if (!tick(lib, ws[i], t, runs[i].seed, nullptr)) live[i] = false;
          else got[i].push_back(snap(lib, ws[i]));
        }
    bool ok = true;
    for (size_t i = 0; i < runs.size(); ++i) ok &= got[i] == ref[i];
    expect(ok, "interleaved worlds match solo runs");
    for (World *w : ws) lib.destroy(w);
  }

  // 3. Threads, each with its own Lib, all running the same run.
  {
    std::vector<uint64_t> ref = solo(lib, runs[2], nullptr);
    std::vector<std::vector<uint64_t>> got(6);
    std::vector<std::thread> ts;
    for (int k = 0; k < 6; ++k)
      ts.emplace_back([&, k] {
        Lib mine(so);
        got[k] = solo(mine, runs[2], nullptr);
      });
    for (auto &t : ts) t.join();
    bool ok = true;
    for (auto &g : got) ok &= g == ref;
    expect(ok, "six threads with their own Lib match the solo run");
  }

  // 4. copy() as save/restore.
  {
    World *a = lib.create(), *save = lib.create();
    begin(lib, a, runs[0]);
    for (long t = 0; t < 900; ++t) tick(lib, a, t, 1, nullptr);
    lib.copy(save, a);
    for (long t = 900; t < 1800; ++t) tick(lib, a, t, 1, nullptr);
    uint64_t first = snap(lib, a);
    lib.copy(a, save);
    for (long t = 900; t < 1800; ++t) tick(lib, a, t, 2, nullptr);
    uint64_t other = snap(lib, a);
    lib.copy(a, save);
    for (long t = 900; t < 1800; ++t) tick(lib, a, t, 1, nullptr);
    expect(snap(lib, a) == first && first != other, "copy() restores a world exactly");
    lib.destroy(a);
    lib.destroy(save);
  }
  printf("region %zu bytes\n", lib.region_size());
  return fails ? 1 : 0;
}
