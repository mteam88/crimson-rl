// Many crimson-core worlds in one process.
//
// libcrimson_core.so (core/build.py) reaches every game global through its GOT. A Lib is one
// dlopen'd copy of it (one per thread: each copy has its own code and GOT, so threads never
// share one). A World is a private copy of that library's state region. use() points the GOT's
// state slots at a world, so the unmodified gameplay code then reads and writes that world.
// Every Lib loads the same file, so a world from one Lib runs in any other: use() adopts it,
// rebasing the pointers the state holds into the library image (vtables, quest builders, strings).
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
  // Makes w the world the gameplay code sees. Cheap when it already is current; adopting a
  // world another Lib last ran scans it once.
  void use(World *w);
  // Points the gameplay code back at the library's own state. Do it when done with a world that
  // may be destroyed elsewhere: the library's static destructors (at exit) run through the GOT.
  void release();

  // A state symbol's address in world w (or nullptr if it isn't state).
  void *addr(const World *w, const char *symbol) const;
  // A state symbol's offset in the region, for repeated lookups; -1 if it isn't state.
  ptrdiff_t offset(const char *symbol) const;
  // A symbol's size in bytes (from the dynamic symbol table), 0 if unknown.
  size_t size(const char *symbol) const;
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
  size_t image_size_ = 0;  // the loaded image spans [base_, base_ + image_size_)
  std::vector<char> pristine_;
  uint64_t current_ = 0;  // id of the world the GOT points at (ids are never reused, unlike addresses)
  void *sym(const char *name) const;
};

// How many times a Lib adopted a world another Lib last ran (each scans the world once).
uint64_t adoptions();

struct World {
  char *block;      // region_size() bytes, mmap'd so untouched pages cost nothing
  uintptr_t image;  // base of the library image its pointers point into
  uint64_t id;
};

}  // namespace crimson
