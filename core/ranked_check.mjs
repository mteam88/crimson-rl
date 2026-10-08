// Checks a transport the way crimson.land's leaderboard does (upstream service/src/verify.ts): plays it through the
// WASM core with every tick's controls and aim checked against the ranked view, and derives the run's result.
// Prints {ok, reason, ticks, result} as JSON; exits 1 unless the run would rank (verified and finished in death).
// Usage: node core/ranked_check.mjs <core.wasm> < transport
import fs from "node:fs";
import { decode, field, init, loadCore, step } from "../upstream/crimson/crimson-core/checks/engine.mjs";

const VIEW_W = 1024, VIEW_H = 768, TERRAIN_SIZE = 1024, PAD_AIM_REACH = 96 + 42, AIM_SLACK = 0.01;
const MOVE_KEYS_PRESENT = 1 << 3, MOVE_MODE_PRESENT = 1 << 8, MOVE_MODE_SHIFT = 9;
const AIM_SCHEME_PRESENT = 1 << 12, AIM_SCHEME_SHIFT = 13;
const GAME_OVER = 0x07;

const e = loadCore(process.argv[2]);
const { config, records } = decode(fs.readFileSync(0));
if (config.readUInt32LE(4) !== 1) throw Error("only Survival is checked here");
init(e, config);

let camera = null;
const updateCamera = () => {
  if (e.portable_player_health() > 0) camera = { x: VIEW_W / 2 - e.portable_player_x(), y: VIEW_H / 2 - e.portable_player_y() };
  if (camera === null) return;
  const x = camera.x + e.portable_shake_x(), y = camera.y + e.portable_shake_y();
  camera = { x: Math.max(Math.min(x, -1), VIEW_W - TERRAIN_SIZE), y: Math.max(Math.min(y, -1), VIEW_H - TERRAIN_SIZE) };
};
const inView = (x, y) => -AIM_SLACK <= x && x <= VIEW_W + AIM_SLACK && -AIM_SLACK <= y && y <= VIEW_H + AIM_SLACK;
const check = (r) => {
  const ax = r.readFloatLE(8), ay = r.readFloatLE(12), flags = r.readUInt32LE(16);
  const moveMode = flags & MOVE_MODE_PRESENT ? (flags >>> MOVE_MODE_SHIFT) & 7 : flags & MOVE_KEYS_PRESENT ? 2 : 3;
  const raw = (flags >>> AIM_SCHEME_SHIFT) & 7;
  const aim = flags & AIM_SCHEME_PRESENT ? (raw === 7 ? -1 : raw) : 0;
  if (moveMode < 1 || moveMode > 4 || aim < 0 || aim > 4) return "computer or unknown controls";
  if (moveMode === 4) return "point-click movement is not checked here";
  if (aim === 0 && !inView(ax + camera.x, ay + camera.y)) return `aim (${ax}, ${ay}) outside the view at camera (${camera.x}, ${camera.y})`;
  if (aim === 4 && Math.hypot(ax, ay) > PAD_AIM_REACH + AIM_SLACK) return "pad aim beyond its reach";
  return null;
};

let reason = null, tick = 0;
updateCamera();
for (; tick < records.length && !reason; tick++) {
  const bad = check(records[tick]);
  if (bad) reason = `tick ${tick}: ${bad}`;
  else if (!step(e, records[tick])) reason = `tick ${tick}: an illegal command or a tick past the run's end`;
  else updateCamera();
}
e.portable_snapshot();
const result = {
  outcome: field(e, "globals.game_state_pending") === GAME_OVER ? "death" : "incomplete",
  experience: field(e, "players[0].experience"),
  elapsed_ms: field(e, "globals.run_elapsed_ms") | 0,
  kills: field(e, "globals.creature_kill_count"),
  health: field(e, "players[0].health", true),
  pending_perks: field(e, "globals.perk_pending_count"),
};
if (!reason && result.outcome !== "death") reason = "unfinished: a ranked Survival run ends in death";
console.log(JSON.stringify({ ok: !reason, reason, ticks: records.length, result }));
process.exit(reason ? 1 : 0);
