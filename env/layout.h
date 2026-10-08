/* Observation and action layout of the Crimsonland env (env/env.cpp). C, so PufferLib's binding.c can
 * include it; train/layout.py parses the #defines, so this file is the one source of truth.
 *
 * One observation is CR_OBS_SIZE floats, sections in this order:
 *   scalars    CR_SCALARS          the player, timers, run progress (env.cpp write_scalars lists them)
 *   ids        CR_IDS              categorical, as floats: weapon, alt weapon, the 7 perk choices (0 = none)
 *   perks      CR_PERKS            count of each perk id taken
 *   creatures  CR_CREATURES rows   nearest live creatures first, CR_CREATURE_F each
 *   shots      CR_SHOTS rows       hostile projectiles first, then the player's, nearest first, CR_SHOT_F each
 *   bonuses    CR_BONUSES rows     bonus pickups on the ground, CR_BONUS_F each
 *   local      CR_LOCAL_C x CR_LOCAL x CR_LOCAL   grid centered on the player, CR_LOCAL_CELL world units a cell
 *   global     CR_GLOBAL_C x CR_GLOBAL x CR_GLOBAL  grid over the arena and its spawn margin
 * In every row, field 0 is 1 for a present entity and the last *_IDS fields are categorical ids
 * (0 = none) for the encoder to embed; vocabulary sizes are the *_VOCAB defines.
 * Positions are relative to the player in world units / CR_SCALE, axes as the world's (x right, y down). */
#ifndef CRIMSON_ENV_LAYOUT_H
#define CRIMSON_ENV_LAYOUT_H

#define CR_SCALE 512.0f

#define CR_SCALARS 64
#define CR_IDS 9
#define CR_WEAPON_VOCAB 64
#define CR_PERK_VOCAB 64
#define CR_PERKS 64

#define CR_CREATURES 96
#define CR_CREATURE_F 28
#define CR_CREATURE_IDS 3 /* type, ai mode, ranged projectile type */
#define CR_CREATURE_TYPE_VOCAB 8
#define CR_AI_VOCAB 16
#define CR_SHOT_TYPE_VOCAB 64 /* projectile types 1..0x2D, secondary (rocket) types at 48 + type */

#define CR_SHOTS 64
#define CR_SHOT_F 14
#define CR_SHOT_IDS 1

#define CR_BONUSES 16
#define CR_BONUS_F 10
#define CR_BONUS_IDS 2 /* bonus id, weapon id for weapon bonuses */
#define CR_BONUS_VOCAB 16

#define CR_LOCAL 24
#define CR_LOCAL_CELL 24.0f
#define CR_LOCAL_C 5 /* creatures, their health, hostile shots, bonuses, outside the arena */
#define CR_GLOBAL 16
#define CR_GLOBAL_LO -128.0f /* the global grid spans [LO, HI] on both axes */
#define CR_GLOBAL_HI 1152.0f
#define CR_GLOBAL_C 3 /* creatures, their health, the player */

#define CR_OFF_SCALARS 0
#define CR_OFF_IDS (CR_OFF_SCALARS + CR_SCALARS)
#define CR_OFF_PERKS (CR_OFF_IDS + CR_IDS)
#define CR_OFF_CREATURES (CR_OFF_PERKS + CR_PERKS)
#define CR_OFF_SHOTS (CR_OFF_CREATURES + CR_CREATURES * CR_CREATURE_F)
#define CR_OFF_BONUSES (CR_OFF_SHOTS + CR_SHOTS * CR_SHOT_F)
#define CR_OFF_LOCAL (CR_OFF_BONUSES + CR_BONUSES * CR_BONUS_F)
#define CR_OFF_GLOBAL (CR_OFF_LOCAL + CR_LOCAL_C * CR_LOCAL * CR_LOCAL)
#define CR_OBS_SIZE (CR_OFF_GLOBAL + CR_GLOBAL_C * CR_GLOBAL * CR_GLOBAL)

/* Actions, one discrete choice per head:
 *   move    0 = stand, k = walk toward angle (k - 1) * 360 / (CR_MOVE - 1) degrees
 *   aim     aim at angle k * 360 / CR_AIM degrees, CR_AIM_DIST from the player
 *   fire    hold the trigger
 *   reload  press reload
 *   perk    0 = nothing, 1 = open the perk menu (reveals the choices; the trigger is released that tick),
 *           2 + i = take choice i. Ignored when no perk is pending or the choice doesn't exist. */
#define CR_NUM_ATNS 5
#define CR_MOVE 17
#define CR_AIM 72
#define CR_AIM_DIST 200.0f
#define CR_FIRE 2
#define CR_RELOAD 2
#define CR_PERK 9

#endif
