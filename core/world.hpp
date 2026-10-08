// Many crimson-core worlds in one process.
//
// libcrimson_core.so (core/build.py) reaches every game global through its GOT. A Lib is one
// dlopen'd copy of it (one per thread: each copy has its own code and GOT, so threads never
// share one). A World is a private copy of that library's state region. use() points the GOT's
// state slots at a world, so the unmodified gameplay code then reads and writes that world.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "api.h"

namespace crimson {

struct World;

class Lib {
 public:
  // Loads a private copy of the library at `so_path`.
  explicit Lib(const std::string &so_path);
  ~Lib();
  Lib(const Lib &) = delete;
  Lib &operator=(const Lib &) = delete;

  // A fresh world: the state as it was right after the library loaded.
  World *create();
  void destroy(World *w);
  // Copies all of src's state into dst (both from this Lib). A save/restore for search.
  void copy(World *dst, const World *src);
  // Makes w the world the gameplay code sees. Cheap when it already is.
  void use(World *w);
  World *current() const { return current_; }

  // A state symbol's address in world w (or nullptr if it isn't state).
  void *addr(const World *w, const char *symbol) const;
  // A state symbol's offset in the region, for repeated lookups; -1 if it isn't state.
  ptrdiff_t offset(const char *symbol) const;
  size_t region_size() const { return hi_ - lo_; }

  // The core's API, bound to this copy. Call use() first.
  int (*init)(uint32_t seed, int mode, int major, int minor);
  int (*step)(int command, int argument);
  int (*step_many)(uint32_t count);
  int (*snapshot)();
  uintptr_t (*config)();
  uintptr_t (*input)();
  uintptr_t (*output)();
  uintptr_t (*commands)();
  uintptr_t (*probe)();
  float (*player_x)();
  float (*player_y)();

 private:
  void *handle_ = nullptr;
  std::string path_;
  uintptr_t base_ = 0;
  char *lo_ = nullptr, *hi_ = nullptr;  // the state region: .data through .bss
  char *skip_lo_ = nullptr, *skip_hi_ = nullptr;  // `output`: per-library scratch, not state
  std::vector<uintptr_t *> slots_;  // GOT slots that point into the region
  std::vector<uintptr_t> slot_off_;
  std::vector<char> pristine_;
  World *current_ = nullptr;
  void *sym(const char *name) const;
  void restore_own();
};

struct World {
  char *block;  // region_size() bytes, mmap'd so untouched pages cost nothing
  Lib *lib;
};

}  // namespace crimson
