#include "world.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace crimson {
namespace {

[[noreturn]] void die(const char *what, const std::string &detail = "") {
  fprintf(stderr, "crimson world: %s %s\n", what, detail.c_str());
  abort();
}

std::vector<char> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) die("cannot read", path);
  return {std::istreambuf_iterator<char>(f), {}};
}

// dlopen returns the already-loaded copy for a path it has seen, so each Lib loads its own file.
std::string private_copy(const std::string &path) {
  static std::atomic<int> counter{0};
  char name[128];
  snprintf(name, sizeof name, "/dev/shm/crimson-core-%d-%d.so", getpid(), counter++);
  std::vector<char> bytes = read_file(path);
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0700);
  if (fd < 0 || write(fd, bytes.data(), bytes.size()) != (ssize_t)bytes.size() || close(fd))
    die("cannot write", name);
  return name;
}

const size_t PAGE = 4096;
std::atomic<uint64_t> next_world_id{1};
std::atomic<uint64_t> adoption_count{0};

}  // namespace

Lib::Lib(const std::string &so_path) : path_(so_path) {
  std::string copy = private_copy(so_path);
  // DEEPBIND: the copy's own references resolve inside the copy, never to a same-named symbol elsewhere.
  handle_ = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  unlink(copy.c_str());
  if (!handle_) die("dlopen failed", dlerror());
  link_map *map = nullptr;
  if (dlinfo(handle_, RTLD_DI_LINKMAP, &map) || !map) die("dlinfo failed");
  base_ = map->l_addr;

  // Section and relocation tables come from the file; the loaded image is the same file at base_.
  std::vector<char> file = read_file(so_path);
  auto *eh = reinterpret_cast<const Elf64_Ehdr *>(file.data());
  auto *sh = reinterpret_cast<const Elf64_Shdr *>(file.data() + eh->e_shoff);
  const char *shstr = file.data() + sh[eh->e_shstrndx].sh_offset;
  const Elf64_Shdr *data = nullptr, *bss = nullptr, *rela = nullptr, *dynsym = nullptr, *dynstr = nullptr;
  for (int i = 0; i < eh->e_shnum; ++i) {
    const char *n = shstr + sh[i].sh_name;
    if (!strcmp(n, ".data")) data = &sh[i];
    if (!strcmp(n, ".bss")) bss = &sh[i];
    if (!strcmp(n, ".rela.dyn")) rela = &sh[i];
    if (!strcmp(n, ".dynsym")) dynsym = &sh[i];
    if (!strcmp(n, ".dynstr")) dynstr = &sh[i];
  }
  if (!data || !bss || !rela || !dynsym || !dynstr) die("missing sections in", so_path);
  if (data->sh_addr > bss->sh_addr) die(".data must precede .bss");
  lo_ = reinterpret_cast<char *>(base_ + data->sh_addr);
  hi_ = reinterpret_cast<char *>(base_ + bss->sh_addr + bss->sh_size);

  auto *syms = reinterpret_cast<const Elf64_Sym *>(file.data() + dynsym->sh_offset);
  const char *strs = file.data() + dynstr->sh_offset;
  auto in_region = [&](uintptr_t a) { return a >= (uintptr_t)lo_ && a < (uintptr_t)hi_; };

  if (char *out = static_cast<char *>(sym("output"))) {
    skip_lo_ = out;
    skip_hi_ = out + 262144 * sizeof(uint32_t);
  }
  auto *r = reinterpret_cast<const Elf64_Rela *>(file.data() + rela->sh_offset);
  size_t n = rela->sh_size / sizeof(Elf64_Rela);
  for (size_t i = 0; i < n; ++i) {
    uint32_t type = ELF64_R_TYPE(r[i].r_info);
    const Elf64_Sym &s = syms[ELF64_R_SYM(r[i].r_info)];
    uintptr_t where = base_ + r[i].r_offset;
    uintptr_t target;
    if (type == R_X86_64_GLOB_DAT || type == R_X86_64_64) {
      if (s.st_shndx == SHN_UNDEF) continue;
      target = base_ + s.st_value + (type == R_X86_64_64 ? r[i].r_addend : 0);
    } else if (type == R_X86_64_RELATIVE) {
      target = base_ + r[i].r_addend;
    } else {
      continue;
    }
    // A pointer stored inside the state to the state would point at the template in every world.
    if (in_region(where) && in_region(target)) die("state holds a load-time pointer into state:", strs + s.st_name);
    if (in_region(where) || !in_region(target)) continue;
    // RELATIVE ones outside the region are local data (Zig's math runtime) the game never reaches.
    if (type == R_X86_64_RELATIVE) continue;
    if (type != R_X86_64_GLOB_DAT) die("non-GOT reference to state:", strs + s.st_name);
    if (skip_lo_ && target >= (uintptr_t)skip_lo_ && target < (uintptr_t)skip_hi_) continue;
    slots_.push_back(reinterpret_cast<uintptr_t *>(where));
    slot_off_.push_back(target - (uintptr_t)lo_);
  }
  if (slots_.size() < 1000) die("too few state GOT slots; is this the -fPIC world build?");
  auto *ph = reinterpret_cast<const Elf64_Phdr *>(file.data() + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; ++i)
    if (ph[i].p_type == PT_LOAD) image_size_ = std::max<size_t>(image_size_, ph[i].p_vaddr + ph[i].p_memsz);
  pristine_.assign(lo_, hi_);

  auto fn = [&](const char *name) {
    void *p = dlsym(handle_, name);
    if (!p) die("missing export", name);
    return p;
  };
  init = reinterpret_cast<decltype(init)>(fn("portable_init"));
  step = reinterpret_cast<decltype(step)>(fn("portable_step"));
  step_many = reinterpret_cast<decltype(step_many)>(fn("portable_step_many"));
  snapshot = reinterpret_cast<decltype(snapshot)>(fn("portable_snapshot"));
  config = reinterpret_cast<decltype(config)>(fn("portable_config"));
  input = reinterpret_cast<decltype(input)>(fn("portable_input"));
  output = reinterpret_cast<decltype(output)>(fn("portable_output"));
  commands = reinterpret_cast<decltype(commands)>(fn("portable_commands"));
  probe = reinterpret_cast<decltype(probe)>(fn("portable_probe"));
  player_x = reinterpret_cast<decltype(player_x)>(fn("portable_player_x"));
  player_y = reinterpret_cast<decltype(player_y)>(fn("portable_player_y"));
}

Lib::~Lib() {
  // Static destructors run at dlclose and reach their globals through the GOT: give them the library's own.
  release();
  if (handle_) dlclose(handle_);
}

void Lib::release() {
  for (size_t i = 0; i < slots_.size(); ++i) *slots_[i] = reinterpret_cast<uintptr_t>(lo_) + slot_off_[i];
  current_ = 0;
}

void *Lib::sym(const char *name) const { return dlsym(handle_, name); }

World *Lib::create() {
  size_t size = hi_ - lo_;
  // Same offset within a page as the library's own region, so every global keeps its alignment.
  size_t lead = reinterpret_cast<uintptr_t>(lo_) & (PAGE - 1);
  void *map = mmap(nullptr, lead + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) die("mmap failed");
  char *b = static_cast<char *>(map) + lead;
  // The template is mostly zero .bss; leave those pages untouched so they stay uncommitted.
  size_t skip_a = skip_lo_ ? skip_lo_ - lo_ : size, skip_b = skip_lo_ ? skip_hi_ - lo_ : size;
  for (size_t p = 0; p < size;) {
    size_t end = std::min(size, (p + lead) / PAGE * PAGE + PAGE - lead);
    if (!(p >= skip_a && end <= skip_b)) {
      const char *src = pristine_.data() + p;
      bool zero = true;
      for (size_t i = 0; i < end - p && zero; ++i) zero = src[i] == 0;
      if (!zero) memcpy(b + p, src, end - p);
    }
    p = end;
  }
  return new World{b, base_, next_world_id++};
}

void Lib::destroy(World *w) {
  if (current_ == w->id) release();
  size_t lead = reinterpret_cast<uintptr_t>(lo_) & (PAGE - 1);
  munmap(w->block - lead, lead + (hi_ - lo_));
  delete w;
}

void Lib::copy(World *dst, const World *src) {
  size_t size = hi_ - lo_;
  if (!skip_lo_) {
    memcpy(dst->block, src->block, size);
    dst->image = src->image;
    return;
  }
  size_t a = skip_lo_ - lo_, b = skip_hi_ - lo_;
  memcpy(dst->block, src->block, a);
  memcpy(dst->block + b, src->block + b, size - b);
  dst->image = src->image;
}

void Lib::use(World *w) {
  if (w->image != base_) {
    // Another Lib ran it: move every pointer into that Lib's image to the same place in ours.
    uintptr_t from = w->image;
    size_t size = hi_ - lo_;
    for (size_t off = (8 - (reinterpret_cast<uintptr_t>(lo_) & 7)) & 7; off + 8 <= size; off += 8) {
      uintptr_t v;
      memcpy(&v, w->block + off, 8);
      if (v - from < image_size_) {
        v = v - from + base_;
        memcpy(w->block + off, &v, 8);
      }
    }
    w->image = base_;
    adoption_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (current_ == w->id) return;
  uintptr_t base = reinterpret_cast<uintptr_t>(w->block);
  for (size_t i = 0; i < slots_.size(); ++i) *slots_[i] = base + slot_off_[i];
  current_ = w->id;
}

uint64_t adoptions() { return adoption_count.load(); }

ptrdiff_t Lib::offset(const char *symbol) const {
  char *p = static_cast<char *>(sym(symbol));
  if (!p || p < lo_ || p >= hi_) return -1;
  return p - lo_;
}

size_t Lib::size(const char *symbol) const {
  void *p = sym(symbol);
  Dl_info info;
  ElfW(Sym) *entry = nullptr;
  if (!p || !dladdr1(p, &info, reinterpret_cast<void **>(&entry), RTLD_DL_SYMENT) || !entry) return 0;
  return entry->st_size;
}

void *Lib::addr(const World *w, const char *symbol) const {
  ptrdiff_t off = offset(symbol);
  return off < 0 ? nullptr : w->block + off;
}

}  // namespace crimson
