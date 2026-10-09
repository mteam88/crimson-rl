#include "env.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "cl_build.h"
#include "crimsonland_types.h"
#include "crimsonland_ui_state_owner.h"
#include "world.hpp"

namespace crimson {
namespace {

std::string core_path;
std::mutex core_path_lock;

[[noreturn]] void die(const char *what, const char *detail = "") {
  fprintf(stderr, "crimson env: %s %s\n", what, detail);
  abort();
}

// Where the game's state lives in a world. The same in every Lib: they all load one file.
struct Layout {
  ptrdiff_t player, creatures, shots, rockets, bonuses, pending, dirty, choices, menu_open, elapsed, stage, active,
      shock_links, timers[5], shake, weapons;
};
Layout layout;
std::once_flag layout_once;

void resolve(const Lib &lib) {
  auto off = [&](const char *name, size_t expect) {
    ptrdiff_t o = lib.offset(name);
    if (o < 0) die("state symbol missing:", name);
    size_t n = lib.size(name);
    if (expect && n && n != expect) {
      char msg[96];
      snprintf(msg, sizeof msg, "%s is %zu bytes, headers say %zu", name, n, expect);
      die("layout mismatch:", msg);
    }
    return o;
  };
  layout.player = off("player_state_table", 2 * sizeof(player_state_t));
  layout.creatures = off("creature_pool", 385 * sizeof(creature_t));
  layout.shots = off("projectile_pool", sizeof(projectile_pool_t));
  layout.rockets = off("secondary_projectile_pool", sizeof(secondary_projectile_pool_t));
  layout.bonuses = off("bonus_pool", sizeof(bonus_pool_t));
  layout.pending = off("perk_pending_count", 0);
  layout.dirty = off("perk_choices_dirty", 0);
  layout.choices = off("perk_choice_ids", 7 * sizeof(int));
  layout.menu_open = off("perk_menu_open", 0);  // host.cpp's: the menu opened in the last tick
  layout.elapsed = off("run_elapsed_ms", 0);
  layout.stage = off("survival_spawn_stage", 0);
  layout.active = off("creature_active_count", 0);
  layout.shock_links = off("shock_chain_links_left", 0);
  const char *timers[5] = {"bonus_weapon_power_up_timer", "bonus_reflex_boost_timer", "bonus_freeze_timer",
                           "bonus_energizer_timer", "bonus_double_xp_timer"};
  for (int i = 0; i < 5; ++i) layout.timers[i] = off(timers[i], 0);  // aliases into a larger blob: no size of their own
  layout.weapons = off("weapon_table", 0);  // 64 rows and the next row's 4-byte ammo class
  layout.shake = off("ui_mouse_blocked", 0) + offsetof(ui_runtime_state_original_t, camera_shake_offset_value);
}

uint64_t splitmix(uint64_t &s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

constexpr uint32_t FIRE = 1, RELOAD = 65536;
constexpr uint32_t SCHEMES = 0x100 | (3u << 9) | 0x1000;  // pad movement, mouse aim: both human schemes
constexpr float PI = 3.14159265358979f;
constexpr float TERRAIN_SIZE = 1024;  // the arena

float lg(float x) { return logf(1.0f + std::max(x, 0.0f)); }

// Where a body moving in a straight line from (dx, dy) relative to the player passes closest to it: the distance
// then, and how far the body travels until then. Neither depends on the units its velocity is in. A body moving
// away is closest now.
void closest_approach(float dx, float dy, float vx, float vy, float *dist, float *path) {
  float v2 = vx * vx + vy * vy, t = v2 > 0 ? std::max(0.0f, -(dx * vx + dy * vy) / v2) : 0;
  float cx = dx + vx * t, cy = dy + vy * t;
  *dist = sqrtf(cx * cx + cy * cy);
  *path = t * sqrtf(v2);
}

}  // namespace

void set_core_library(const std::string &path) {
  std::lock_guard<std::mutex> g(core_path_lock);
  core_path = path;
}

static Lib &thread_lib() {
  thread_local std::unique_ptr<Lib> lib;
  if (!lib) {
    std::string path;
    {
      std::lock_guard<std::mutex> g(core_path_lock);
      if (core_path.empty())
        if (const char *e = getenv("CRIMSON_CORE_SO")) core_path = e;
      path = core_path;
    }
    if (path.empty()) die("no core library: call set_core_library or set CRIMSON_CORE_SO");
    lib = std::make_unique<Lib>(path);
    std::call_once(layout_once, [&] { resolve(*lib); });
  }
  return *lib;
}

Env::Env(const EnvConfig &config, uint64_t rng_seed) : config_(config), rng_(rng_seed) {
  if (config_.mode != 1 && config_.mode != 2) die("unsupported mode");
  if (config_.repeat < 1) die("repeat must be >= 1");
  world_ = thread_lib().create();
}

Env::~Env() {
  if (world_) thread_lib().destroy(world_);
}

Lib &Env::lib() {
  Lib &l = thread_lib();
  l.use(world_);
  return l;
}

// Every public call leaves the thread's Lib pointing at its own state, never at this world, which
// another thread may run or destroy next.
struct Release {
  ~Release() { thread_lib().release(); }
};

uint32_t Env::next_seed() { return (uint32_t)splitmix(rng_); }

int Env::experience() const {
  return reinterpret_cast<const player_state_t *>(world_->block + layout.player)->experience;
}

float Env::health() const {
  return reinterpret_cast<const player_state_t *>(world_->block + layout.player)->health;
}

void Env::debug_set_weapon(int id) {
  auto *p = reinterpret_cast<player_state_t *>(world_->block + layout.player);
  const weapon_stats_t &w = reinterpret_cast<const weapon_stats_t *>(world_->block + layout.weapons)[id];
  p->weapon_id = id;
  p->clip_size = w.clip_size;
  p->ammo = w.clip_size;
  p->reload_timer = 0;
  p->shot_cooldown = 0;
}

uint64_t Env::state_hash() const {
  // Pointers into the library's image differ between Libs: hash the game's tables, which hold none.
  uint64_t h = 1469598103934665603ull;
  auto add = [&](ptrdiff_t off, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)world_->block[off + i]) * 1099511628211ull;
  };
  add(layout.player, sizeof(player_state_t));
  add(layout.creatures, 385 * sizeof(creature_t));
  add(layout.shots, sizeof(projectile_pool_t));
  add(layout.bonuses, sizeof(bonus_pool_t));
  return h;
}

void Env::copy_from(const Env &src) {
  thread_lib().copy(world_, src.world_);
  rng_ = src.rng_;
  seed_ = src.seed_;
  ticks_ = src.ticks_;
  last_xp_ = src.last_xp_;
  return_ = src.return_;
  memcpy(last_action_, src.last_action_, sizeof last_action_);
}

bool Env::alive() const {
  return reinterpret_cast<const player_state_t *>(world_->block + layout.player)->health > 0;
}

void Env::begin(uint32_t seed) {
  Lib &l = lib();
  auto *cfg = reinterpret_cast<PortableConfig *>(l.config());
  memset(cfg, 0, sizeof *cfg);
  // The ranked Survival profile (upstream host/ranked.inc ranked_spec).
  cfg->seed = seed;
  cfg->mode = config_.mode;
  cfg->major = cfg->minor = 1;
  cfg->unlock = cfg->unlock_full = 50;
  cfg->detail = 5;
  if (!l.init(seed, config_.mode, 1, 1)) die("portable_init failed");
  seed_ = seed;
  ticks_ = 0;
  memset(last_action_, 0, sizeof last_action_);
  last_xp_ = experience();
  return_ = 0;
  if (config_.record) {
    transport_.assign(reinterpret_cast<uint8_t *>(cfg), reinterpret_cast<uint8_t *>(cfg) + sizeof *cfg);
  }
}

void Env::reset(uint32_t seed, float *obs) {
  Release r;
  begin(seed);
  observe(obs);
}

bool Env::tick(const PortableInput &in, const PortableCommand *cmd) {
  Lib &l = lib();
  *reinterpret_cast<PortableInput *>(l.input()) = in;
  int ok = cmd ? l.step(cmd->type, cmd->argument) : l.step(0, 0);
  if (ok && config_.record) {
    auto put = [&](const void *p, size_t n) {
      transport_.insert(transport_.end(), (const uint8_t *)p, (const uint8_t *)p + n);
    };
    put(&in, sizeof in);
    uint32_t count = cmd ? 1 : 0;
    put(&count, 4);
    if (cmd) put(cmd, sizeof *cmd);
  }
  ticks_ += ok != 0;
  return ok;
}

void Env::finish() {
  // A run ranks only once it has finished: idle through the death timer and the game's run-down until
  // the core refuses a tick, as the recording game would. Shots still in flight can score meanwhile.
  if (config_.record && !alive()) {
    PortableInput idle{};
    idle.flags = SCHEMES;
    for (int i = 0; i < 600; ++i) {
      auto *p = reinterpret_cast<const player_state_t *>(world_->block + layout.player);
      idle.aim_x = p->pos_x, idle.aim_y = p->pos_y;  // the camera stays where it last saw the player
      if (!tick(idle, nullptr)) break;
    }
  }
  int xp = experience();
  stats.runs += 1;
  stats.score += xp;
  stats.ticks += ticks_;
  stats.episode_return += return_;
  if (config_.record) {
    last_transport_.swap(transport_);
    last_score_ = xp;
  }
}

float Env::step(const int *a, float *obs, bool *done) {
  Release release;
  lib();
  char *b = world_->block;
  auto *player = reinterpret_cast<player_state_t *>(b + layout.player);
  int pending = *reinterpret_cast<int *>(b + layout.pending);
  bool dirty = *reinterpret_cast<unsigned char *>(b + layout.dirty) != 0;
  const int *choices = reinterpret_cast<const int *>(b + layout.choices);

  PortableInput in{};
  if (a[0] > 0) {
    float t = (a[0] - 1) * 2 * PI / (CR_MOVE - 1);
    in.move_x = cosf(t);
    in.move_y = sinf(t);
  }
  float aim = a[1] * 2 * PI / CR_AIM;
  in.flags = SCHEMES | (a[2] ? FIRE : 0) | (a[3] ? RELOAD : 0);

  // Perks go through the game's own menu key, as crimson.land's verifier takes them (upstream #589): the menu opens
  // in a tick, and only the next tick may start with a pick; a tick without one closes it. So an open goes on a
  // decision's last tick, and a pick on the first tick of the decision right after an opening. A pick asked for
  // with no menu open opens it instead (choices already revealed are kept, not drawn again), and lands when the
  // next decision asks for it again.
  PortableCommand cmd{0, 0};
  bool can_perk = config_.mode != 2 && pending > 0 && player->health > 0;
  bool menu_open = *reinterpret_cast<const bool *>(b + layout.menu_open);
  bool opening = false;
  if (can_perk && a[4] == 1) {
    opening = true;
  } else if (can_perk && a[4] >= 2) {
    int i = a[4] - 2;
    int n = player->perk_counts[PERK_ID_PERK_MASTER] > 0 ? 7 : player->perk_counts[PERK_ID_PERK_EXPERT] > 0 ? 6 : 5;
    if (i < n && (dirty || choices[i] > 0)) {
      if (menu_open) cmd = {1, i};
      else opening = true, stats.picks_deferred += 1;
    }
  }

  memcpy(last_action_, a, sizeof last_action_);
  bool ended = false, opened = false;
  const PortableCommand open{2, 0};
  for (int r = 0; r < config_.repeat && !ended; ++r) {
    aim_at(aim, &in.aim_x, &in.aim_y);
    bool last = r == config_.repeat - 1;
    PortableInput tin = in;
    if (opening && last) tin.flags &= ~FIRE;  // the menu key is ignored while the trigger is held
    const PortableCommand *c = r == 0 && cmd.type ? &cmd : opening && last ? &open : nullptr;
    bool ok = tick(tin, c);
    opened = opening && last && ok;
    if (!ok) {
      stats.game_errors += 1;
      ended = true;
    } else if (!alive() || (config_.max_ticks && ticks_ >= config_.max_ticks)) {
      ended = true;
    }
  }
  if (cmd.type == 1) stats.perks += 1;
  if (opened && !*reinterpret_cast<const bool *>(b + layout.menu_open)) stats.reveal_failed += 1;

  int xp = experience();
  float reward = (xp - last_xp_) * config_.xp_scale + config_.alive_reward * config_.repeat / 60.0f;
  last_xp_ = xp;
  if (ended && !alive()) reward -= config_.death_penalty;
  return_ += reward;
  *done = ended;
  if (ended) finish();
  if (ended && config_.auto_reset) begin(next_seed());
  observe(obs);
  return reward;
}

// The aim point CR_AIM_DIST along `angle`, pulled back along the ray into the view a ranked run's
// cursor can reach: 1024x768 around the camera the previous tick left, centred on the player plus
// the shake and clamped to the arena (upstream src/crimson/replay/ranked.py RankedTickMonitor).
// The arena is as wide as the view, so the view always spans x in [0, 1024].
void Env::aim_at(float angle, float *x, float *y) const {
  const char *b = world_->block;
  auto *p = reinterpret_cast<const player_state_t *>(b + layout.player);
  const float *shake = reinterpret_cast<const float *>(b + layout.shake);
  float cam_y = 384 - p->pos_y + shake[1];
  if (cam_y > -1) cam_y = -1;
  if (cam_y < 768 - TERRAIN_SIZE) cam_y = 768 - TERRAIN_SIZE;
  const float lo_x = 1, hi_x = 1023, lo_y = 1 - cam_y, hi_y = 767 - cam_y;  // a unit inside the edges
  float dx = cosf(angle), dy = sinf(angle), reach = CR_AIM_DIST;
  float px = std::clamp(p->pos_x, lo_x, hi_x), py = std::clamp(p->pos_y, lo_y, hi_y);
  if (dx > 0) reach = std::min(reach, (hi_x - px) / dx);
  if (dx < 0) reach = std::min(reach, (lo_x - px) / dx);
  if (dy > 0) reach = std::min(reach, (hi_y - py) / dy);
  if (dy < 0) reach = std::min(reach, (lo_y - py) / dy);
  *x = px + reach * dx;
  *y = py + reach * dy;
}

void Env::observe(float *obs) {
  memset(obs, 0, CR_OBS_SIZE * sizeof(float));
  const char *b = world_->block;
  auto *p = reinterpret_cast<const player_state_t *>(b + layout.player);
  auto *creatures = reinterpret_cast<const creature_t *>(b + layout.creatures);
  auto *shots = reinterpret_cast<const projectile_t *>(b + layout.shots);
  auto *rockets = reinterpret_cast<const secondary_projectile_t *>(b + layout.rockets);
  auto *bonuses = reinterpret_cast<const bonus_entry_t *>(b + layout.bonuses);
  int pending = *reinterpret_cast<const int *>(b + layout.pending);
  bool dirty = *reinterpret_cast<const unsigned char *>(b + layout.dirty) != 0;
  const int *choices = reinterpret_cast<const int *>(b + layout.choices);
  float px = p->pos_x, py = p->pos_y;

  // Creatures: live ones, nearest first.
  struct Near {
    float d2;
    int i;
  };
  Near near[385];
  int nc = 0, close128 = 0, close256 = 0;
  for (int i = 0; i < 384; ++i) {
    const creature_t &c = creatures[i];
    if (!c.active || c.health <= 0) continue;
    float dx = c.pos_x - px, dy = c.pos_y - py, d2 = dx * dx + dy * dy;
    near[nc++] = {d2, i};
    close128 += d2 < 128 * 128;
    close256 += d2 < 256 * 256;
  }
  auto by_distance = [](const Near &x, const Near &y) { return x.d2 < y.d2 || (x.d2 == y.d2 && x.i < y.i); };
  int kc = std::min(nc, CR_CREATURES);
  std::partial_sort(near, near + kc, near + nc, by_distance);
  float *row = obs + CR_OFF_CREATURES;
  for (int k = 0; k < kc; ++k, row += CR_CREATURE_F) {
    const creature_t &c = creatures[near[k].i];
    float dx = c.pos_x - px, dy = c.pos_y - py, d = sqrtf(near[k].d2);
    int shot = c.flags & 0x100 ? (int)c.orbit_radius.raw_u32 : c.flags & 0x10 ? PROJECTILE_TYPE_PLASMA_RIFLE : 0;
    float cpa, path;
    closest_approach(dx, dy, c.vel_x, c.vel_y, &cpa, &path);
    float f[CR_CREATURE_F] = {
        1,
        dx / CR_SCALE,
        dy / CR_SCALE,
        d / CR_SCALE,
        d > 0 ? dx / d : 0,
        d > 0 ? dy / d : 0,
        c.vel_x / 2,
        c.vel_y / 2,
        lg(c.health) / 5,
        c.max_health > 0 ? c.health / c.max_health : 0,
        lg(c.max_health) / 5,
        c.size / 64,
        cosf(c.heading),
        sinf(c.heading),
        c.hit_flash_timer,
        c.contact_damage / 10,
        c.move_speed / 2,
        c.attack_cooldown,
        lg(c.reward_value) / 6,
        (c.flags & 0x3) ? 1.0f : 0.0f,
        (c.flags & 0x44) ? 1.0f : 0.0f,
        (c.flags & 0x8) ? 1.0f : 0.0f,
        (c.flags & 0x110) ? 1.0f : 0.0f,
        (c.flags & 0x80) ? 1.0f : 0.0f,
        (c.flags & 0x400) ? 1.0f : 0.0f,
        d > 0 ? -(dx * c.vel_x + dy * c.vel_y) / d / 2 : 0,  // closing speed, units a tick
        cpa / CR_SCALE,
        path / CR_SCALE,
        (float)std::clamp(c.type_id + 1, 0, CR_CREATURE_TYPE_VOCAB - 1),
        (float)std::clamp(c.ai_mode + 1, 0, CR_AI_VOCAB - 1),
        (float)std::clamp(shot, 0, CR_SHOT_TYPE_VOCAB - 1),
    };
    memcpy(row, f, sizeof f);
  }

  // Shots: hostile projectiles (a creature owns them) before the player's, nearest first.
  struct Shot {
    float key;
    int i;  // >= 0 projectile_pool, < 0 secondary pool (-1 - index)
  };
  Shot list[PROJECTILE_POOL_CAPACITY + 0x40];
  int ns = 0, hostile = 0;
  for (int i = 0; i < PROJECTILE_POOL_CAPACITY; ++i) {
    const projectile_t &s = shots[i];
    if (!s.active) continue;
    float dx = s.fields.pos_x - px, dy = s.fields.pos_y - py;
    bool h = s.fields.owner_id >= 0;
    hostile += h;
    list[ns++] = {dx * dx + dy * dy + (h ? 0.0f : 1e12f), i};
  }
  for (int i = 0; i < 0x40; ++i) {
    const secondary_projectile_t &s = rockets[i];
    if (!s.active) continue;
    float dx = s.fields.pos_x - px, dy = s.fields.pos_y - py;
    list[ns++] = {dx * dx + dy * dy + 1e12f, -1 - i};
  }
  int ks = std::min(ns, CR_SHOTS);
  std::partial_sort(list, list + ks, list + ns,
                    [](const Shot &x, const Shot &y) { return x.key < y.key || (x.key == y.key && x.i < y.i); });
  row = obs + CR_OFF_SHOTS;
  for (int k = 0; k < ks; ++k, row += CR_SHOT_F) {
    float x, y, vx, vy, life, radius = 0, damage = 0;
    int type;
    bool h = false, secondary = list[k].i < 0;
    if (!secondary) {
      const projectile_t &s = shots[list[k].i];
      x = s.fields.pos_x, y = s.fields.pos_y, vx = s.fields.vel_x, vy = s.fields.vel_y;
      life = s.fields.life_timer, radius = s.fields.hit_radius, damage = s.fields.damage_pool;
      type = s.fields.type_id;
      h = s.fields.owner_id >= 0;
    } else {
      const secondary_projectile_t &s = rockets[-1 - list[k].i];
      x = s.fields.pos_x, y = s.fields.pos_y, vx = s.fields.vel_x, vy = s.fields.vel_y;
      life = s.life_timer;
      type = 48 + s.fields.type_id;
    }
    float dx = x - px, dy = y - py, d = sqrtf(dx * dx + dy * dy), v = sqrtf(vx * vx + vy * vy);
    float cpa, path;
    closest_approach(dx, dy, vx, vy, &cpa, &path);
    float f[CR_SHOT_F] = {
        1,
        dx / CR_SCALE,
        dy / CR_SCALE,
        d / CR_SCALE,
        v > 0 ? vx / v : 0,
        v > 0 ? vy / v : 0,
        v / 2,
        h ? 1.0f : 0.0f,
        secondary ? 1.0f : 0.0f,
        life,
        radius / 4,
        lg(damage) / 5,
        cpa / CR_SCALE,
        path / CR_SCALE,
        (float)std::clamp(type, 0, CR_SHOT_TYPE_VOCAB - 1),
    };
    memcpy(row, f, sizeof f);
  }

  // Bonuses on the ground.
  row = obs + CR_OFF_BONUSES;
  int nb = 0;
  for (int i = 0; i < 0x10 && nb < CR_BONUSES; ++i) {
    const bonus_entry_t &e = bonuses[i];
    if (e.bonus_id == BONUS_ID_NONE || e.picked) continue;
    float dx = e.time.pos_x - px, dy = e.time.pos_y - py, d = sqrtf(dx * dx + dy * dy);
    bool weapon = e.bonus_id == BONUS_ID_WEAPON;
    float f[CR_BONUS_F] = {
        1,
        dx / CR_SCALE,
        dy / CR_SCALE,
        d / CR_SCALE,
        e.time.time_left / 10,
        e.time.time_max > 0 ? e.time.time_left / e.time.time_max : 0,
        weapon ? 0.0f : lg((float)e.time.amount) / 8,
        0,
        (float)std::clamp((int)e.bonus_id, 0, CR_BONUS_VOCAB - 1),
        weapon ? (float)std::clamp(e.time.amount, 0, CR_WEAPON_VOCAB - 1) : 0.0f,
    };
    memcpy(row, f, sizeof f);
    row += CR_BONUS_F;
    ++nb;
  }

  // Grids. Local: CR_LOCAL cells of CR_LOCAL_CELL around the player. Global: the arena plus spawn margin.
  float *local = obs + CR_OFF_LOCAL, *global = obs + CR_OFF_GLOBAL;
  const int L = CR_LOCAL, G = CR_GLOBAL;
  const float half = L * CR_LOCAL_CELL / 2, gcell = (CR_GLOBAL_HI - CR_GLOBAL_LO) / G;
  auto local_cell = [&](float x, float y) {
    int cx = (int)floorf((x - px + half) / CR_LOCAL_CELL), cy = (int)floorf((y - py + half) / CR_LOCAL_CELL);
    return cx < 0 || cy < 0 || cx >= L || cy >= L ? -1 : cy * L + cx;
  };
  auto global_cell = [&](float x, float y) {
    int cx = (int)floorf((x - CR_GLOBAL_LO) / gcell), cy = (int)floorf((y - CR_GLOBAL_LO) / gcell);
    return cx < 0 || cy < 0 || cx >= G || cy >= G ? -1 : cy * G + cx;
  };
  for (int k = 0; k < nc; ++k) {
    const creature_t &c = creatures[near[k].i];
    float hp = lg(c.health) / 5;
    int i = local_cell(c.pos_x, c.pos_y);
    if (i >= 0) local[0 * L * L + i] += 1, local[1 * L * L + i] += hp;
    i = global_cell(c.pos_x, c.pos_y);
    if (i >= 0) global[0 * G * G + i] += 1, global[1 * G * G + i] += hp;
  }
  for (int i = 0; i < PROJECTILE_POOL_CAPACITY; ++i) {
    const projectile_t &s = shots[i];
    if (!s.active || s.fields.owner_id < 0) continue;
    int c = local_cell(s.fields.pos_x, s.fields.pos_y);
    if (c >= 0) local[2 * L * L + c] += 1;
  }
  for (int i = 0; i < 0x10; ++i) {
    const bonus_entry_t &e = bonuses[i];
    if (e.bonus_id == BONUS_ID_NONE || e.picked) continue;
    int c = local_cell(e.time.pos_x, e.time.pos_y);
    if (c >= 0) local[3 * L * L + c] = 1;
  }
  for (int cy = 0; cy < L; ++cy)
    for (int cx = 0; cx < L; ++cx) {
      float x = px - half + (cx + 0.5f) * CR_LOCAL_CELL, y = py - half + (cy + 0.5f) * CR_LOCAL_CELL;
      local[4 * L * L + cy * L + cx] = x < 0 || y < 0 || x > 1024 || y > 1024;
    }
  // Counts and health sums pile up where creatures crowd (nests): squash them.
  for (int i = 0; i < 2 * L * L; ++i) local[i] = lg(local[i]);
  for (int i = 2 * L * L; i < 3 * L * L; ++i) local[i] = lg(local[i]);
  for (int i = 0; i < 2 * G * G; ++i) global[i] = lg(global[i]);
  {
    int i = global_cell(px, py);
    if (i >= 0) global[2 * G * G + i] = 1;
  }

  // Scalars.
  float s[CR_SCALARS] = {};
  int n = 0;
  s[n++] = px / 1024;
  s[n++] = py / 1024;
  s[n++] = p->health / 100;
  s[n++] = p->max_health / 100;
  s[n++] = p->move_dx / 100;
  s[n++] = p->move_dy / 100;
  s[n++] = cosf(p->heading);
  s[n++] = sinf(p->heading);
  s[n++] = cosf(p->aim_heading);
  s[n++] = sinf(p->aim_heading);
  s[n++] = p->speed_multiplier;
  s[n++] = p->move_speed / 2;
  s[n++] = lg((float)p->experience) / 12;
  s[n++] = p->level / 50.0f;
  s[n++] = p->spread_heat;
  s[n++] = p->clip_size / 30;
  s[n++] = p->clip_size > 0 ? p->ammo / p->clip_size : 0;
  s[n++] = p->ammo / 30;
  s[n++] = p->reload_active;
  s[n++] = p->reload_timer;
  s[n++] = p->reload_timer_max;
  s[n++] = p->shot_cooldown;
  s[n++] = p->alt_clip_size > 0 ? p->alt_ammo / p->alt_clip_size : 0;
  s[n++] = p->alt_reload_active;
  s[n++] = p->speed_bonus_timer / 10;
  s[n++] = p->shield_timer / 10;
  s[n++] = p->fire_bullets_timer / 10;
  s[n++] = p->hot_tempered_timer;
  s[n++] = p->man_bomb_timer;
  s[n++] = p->living_fortress_timer / 10;
  s[n++] = p->fire_cough_timer;
  for (int i = 0; i < 5; ++i) s[n++] = *reinterpret_cast<const float *>(b + layout.timers[i]) / 10;
  s[n++] = pending / 5.0f;
  s[n++] = pending > 0 && !dirty;
  s[n++] = (p->perk_counts[PERK_ID_PERK_MASTER] > 0 ? 7 : p->perk_counts[PERK_ID_PERK_EXPERT] > 0 ? 6 : 5) / 7.0f;
  s[n++] = *reinterpret_cast<const int *>(b + layout.elapsed) / 600000.0f;
  s[n++] = *reinterpret_cast<const int *>(b + layout.stage) / 10.0f;
  s[n++] = *reinterpret_cast<const int *>(b + layout.active) / 384.0f;
  s[n++] = hostile / 16.0f;
  s[n++] = config_.mode == 1;
  s[n++] = config_.mode == 2;
  s[n++] = nc ? sqrtf(near[0].d2) / CR_SCALE : 4;
  s[n++] = close128 / 16.0f;
  s[n++] = close256 / 32.0f;
  s[n++] = nc / 384.0f;
  s[n++] = *reinterpret_cast<const int *>(b + layout.shock_links) / 10.0f;
  s[n++] = (float)(ticks_ % 60) / 60;
  // The weapon in hand and the alternate one: the table's stats, as this run has them.
  for (int id : {p->weapon_id, p->alt_weapon_id}) {
    if (id < 0 || id >= 64) {
      n += 7;
      continue;
    }
    const weapon_stats_t &w = reinterpret_cast<const weapon_stats_t *>(b + layout.weapons)[id];
    s[n++] = w.clip_size / 30.0f;
    s[n++] = w.shot_cooldown;
    s[n++] = w.reload_time / 2;
    s[n++] = w.spread_heat * 4;
    s[n++] = w.projectile_speed / 50;
    s[n++] = lg(w.damage_scale);
    s[n++] = w.pellet_count / 8.0f;
  }
  // The previous decision.
  const int *a = last_action_;
  s[n++] = a[0] == 0;
  s[n++] = a[0] > 0 ? cosf((a[0] - 1) * 2 * PI / (CR_MOVE - 1)) : 0;
  s[n++] = a[0] > 0 ? sinf((a[0] - 1) * 2 * PI / (CR_MOVE - 1)) : 0;
  s[n++] = cosf(a[1] * 2 * PI / CR_AIM);
  s[n++] = sinf(a[1] * 2 * PI / CR_AIM);
  s[n++] = a[2];
  s[n++] = a[3];
  static_assert(CR_SCALARS >= 70, "scalars above outgrew CR_SCALARS");
  memcpy(obs + CR_OFF_SCALARS, s, sizeof s);

  float *ids = obs + CR_OFF_IDS;
  ids[0] = (float)std::clamp(p->weapon_id, 0, CR_WEAPON_VOCAB - 1);
  ids[1] = (float)std::clamp(p->alt_weapon_id, 0, CR_WEAPON_VOCAB - 1);
  for (int i = 0; i < 7; ++i) ids[2 + i] = pending > 0 && !dirty ? (float)std::clamp(choices[i], 0, CR_PERK_VOCAB - 1) : 0;
  for (int i = 0; i < CR_PERKS; ++i) obs[CR_OFF_PERKS + i] = p->perk_counts[i] / 3.0f;
}

}  // namespace crimson
