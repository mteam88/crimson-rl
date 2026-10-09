// A Crimsonland RL environment: one world of crimson-core (core/world.hpp) played by a policy that sees the
// observation in env/layout.h and acts through its discrete heads. Runs on any thread: each thread loads its
// own copy of the core library on first use, and a world moves between threads freely.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "api.h"
#include "layout.h"

namespace crimson {

class Lib;
struct World;

struct EnvConfig {
  int mode = 1;              // 1 Survival, 2 Rush (Quests later)
  int repeat = 2;            // ticks each decision is held for
  long max_ticks = 0;        // end a run after this many ticks; 0 = never
  float xp_scale = 0.01f;    // reward per experience point
  float death_penalty = 1.0f;
  float alive_reward = 0.0f;  // per second alive
  bool record = false;       // keep the run's ticks, for replays (transport())
  bool auto_reset = true;    // start the next run when one ends; off for search, which keeps the ended state
};

// Accumulated over finished runs, for logging; reset by the caller.
struct EnvStats {
  double runs = 0, score = 0, ticks = 0, episode_return = 0, perks = 0, reveal_failed = 0, game_errors = 0,
         picks_deferred = 0;  // picks asked for with no menu open, which opened it instead
};

class Env {
 public:
  explicit Env(const EnvConfig &config, uint64_t rng_seed);
  ~Env();
  Env(const Env &) = delete;
  Env &operator=(const Env &) = delete;

  // Starts a run on `seed` and writes its first observation.
  void reset(uint32_t seed, float *obs);
  // Starts a run on the next seed from this env's own stream.
  void reset(float *obs) { reset(next_seed(), obs); }
  // Plays one decision. Returns its reward; *done is set when the run ended (the next run has then
  // already started, and obs is its first observation).
  float step(const int *actions, float *obs, bool *done);

  // Takes over src's run: its world and the run's progress (not the recording or stats). A save/restore.
  void copy_from(const Env &src);
  // Writes the observation of the current state.
  void observe(float *obs);

  uint32_t seed() const { return seed_; }
  float health() const;
  // A hash of the world's state, for checking that two runs agree.
  uint64_t state_hash() const;
  long ticks() const { return ticks_; }
  int experience() const;
  bool alive() const;
  // The current run as upstream's transport (PortableConfig, then per tick input + commands), when recording.
  const std::vector<uint8_t> &transport() const { return transport_; }
  // The last finished run's transport and score, when recording.
  const std::vector<uint8_t> &last_transport() const { return last_transport_; }
  int last_score() const { return last_score_; }

  EnvStats stats;

 private:
  EnvConfig config_;
  Lib *lib_ = nullptr;
  World *world_ = nullptr;
  uint64_t rng_;
  uint32_t seed_ = 0;
  long ticks_ = 0;
  int last_xp_ = 0;
  int last_action_[CR_NUM_ATNS] = {};
  double return_ = 0;
  std::vector<uint8_t> transport_, last_transport_;
  int last_score_ = 0;

  Lib &lib();  // this thread's Lib, with world_ in use
  uint32_t next_seed();
  void begin(uint32_t seed);
  void finish();
  bool tick(const PortableInput &in, const PortableCommand *cmd);
  void aim_at(float angle, float *x, float *y) const;
};

// Where each thread loads the core library from; defaults to $CRIMSON_CORE_SO.
void set_core_library(const std::string &path);

}  // namespace crimson
