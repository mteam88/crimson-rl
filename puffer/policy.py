"""The Crimsonland policy encoder, and the observation layout read from env/layout.h."""
import re
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F

LAYOUT_H = Path(__file__).resolve().parent.parent / "env" / "layout.h"


def read_layout(path=LAYOUT_H):
    """Every #define in layout.h, evaluated in order (they are integer and float expressions)."""
    text = re.sub(r"/\*.*?\*/", "", Path(path).read_text(), flags=re.S)
    values = {}
    for name, expr in re.findall(r"^#define\s+(CR_\w+)\s+(.+)$", text, flags=re.M):
        expr = re.sub(r"(\d+\.\d*)f\b", r"\1", expr.strip())
        values[name] = eval(expr, {}, dict(values))
    return values


L = read_layout()


class EntitySet(nn.Module):
    """Rows of `fields` floats whose last len(vocabs) are categorical ids: a shared MLP per row, then
    masked max and mean pooling over the present rows (field 0)."""

    def __init__(self, fields, vocabs, dim, embed=8):
        super().__init__()
        self.cont = fields - len(vocabs)
        self.embeds = nn.ModuleList(nn.Embedding(v, embed) for v in vocabs)
        self.mlp = nn.Sequential(
            nn.Linear(self.cont + embed * len(vocabs), dim), nn.GELU(), nn.Linear(dim, dim), nn.GELU())
        self.out = 2 * dim

    def forward(self, rows):
        ids = rows[..., self.cont:].long()
        x = torch.cat([rows[..., :self.cont]] + [e(ids[..., i]) for i, e in enumerate(self.embeds)], dim=-1)
        h = self.mlp(x)
        mask = rows[..., :1] > 0
        count = mask.sum(1).clamp(min=1)
        pooled_max = h.masked_fill(~mask, -1e4).max(1).values
        pooled_max = torch.where(mask.any(1), pooled_max, torch.zeros_like(pooled_max))
        pooled_mean = (h * mask).sum(1) / count
        return torch.cat([pooled_max, pooled_mean], dim=-1)


class CrimsonEncoder(nn.Module):
    def __init__(self, obs_size, hidden_size=256):
        super().__init__()
        assert obs_size == L["CR_OBS_SIZE"], f"obs size {obs_size} != layout.h {L['CR_OBS_SIZE']}"
        self.weapon = nn.Embedding(L["CR_WEAPON_VOCAB"], 16)
        self.perk = nn.Embedding(L["CR_PERK_VOCAB"], 16)
        self.ego = nn.Sequential(nn.Linear(L["CR_SCALARS"] + L["CR_PERKS"] + 2 * 16 + 7 * 16, 256), nn.GELU())
        self.creatures = EntitySet(
            L["CR_CREATURE_F"], [L["CR_CREATURE_TYPE_VOCAB"], L["CR_AI_VOCAB"], L["CR_SHOT_TYPE_VOCAB"]], 128)
        self.shots = EntitySet(L["CR_SHOT_F"], [L["CR_SHOT_TYPE_VOCAB"]], 64)
        self.bonuses = EntitySet(L["CR_BONUS_F"], [L["CR_BONUS_VOCAB"], L["CR_WEAPON_VOCAB"]], 32)
        n, c = L["CR_LOCAL"], L["CR_LOCAL_C"]
        self.local = nn.Sequential(
            nn.Conv2d(c, 32, 3, padding=1), nn.GELU(),
            nn.Conv2d(32, 64, 3, stride=2, padding=1), nn.GELU(),
            nn.Conv2d(64, 64, 3, stride=2, padding=1), nn.GELU(),
            nn.Flatten(), nn.Linear(64 * ((n + 3) // 4) ** 2, 256), nn.GELU())
        g, gc = L["CR_GLOBAL"], L["CR_GLOBAL_C"]
        self.glob = nn.Sequential(
            nn.Conv2d(gc, 16, 3, stride=2, padding=1), nn.GELU(),
            nn.Conv2d(16, 32, 3, stride=2, padding=1), nn.GELU(),
            nn.Flatten(), nn.Linear(32 * ((g + 3) // 4) ** 2, 128), nn.GELU())
        width = 256 + self.creatures.out + self.shots.out + self.bonuses.out + 256 + 128
        self.mix = nn.Sequential(nn.Linear(width, hidden_size), nn.GELU())

    def forward(self, obs):
        obs = obs.view(obs.shape[0], -1).float()
        B = obs.shape[0]

        def section(off, n):
            return obs[:, L[off]:L[off] + n]

        ids = section("CR_OFF_IDS", L["CR_IDS"]).long()
        ego = torch.cat([
            section("CR_OFF_SCALARS", L["CR_SCALARS"]),
            section("CR_OFF_PERKS", L["CR_PERKS"]),
            self.weapon(ids[:, :2]).flatten(1),
            self.perk(ids[:, 2:9]).flatten(1),
        ], dim=1)
        creatures = section("CR_OFF_CREATURES", L["CR_CREATURES"] * L["CR_CREATURE_F"]).view(
            B, L["CR_CREATURES"], L["CR_CREATURE_F"])
        shots = section("CR_OFF_SHOTS", L["CR_SHOTS"] * L["CR_SHOT_F"]).view(B, L["CR_SHOTS"], L["CR_SHOT_F"])
        bonuses = section("CR_OFF_BONUSES", L["CR_BONUSES"] * L["CR_BONUS_F"]).view(
            B, L["CR_BONUSES"], L["CR_BONUS_F"])
        n, c = L["CR_LOCAL"], L["CR_LOCAL_C"]
        local = section("CR_OFF_LOCAL", c * n * n).view(B, c, n, n)
        g, gc = L["CR_GLOBAL"], L["CR_GLOBAL_C"]
        glob = section("CR_OFF_GLOBAL", gc * g * g).view(B, gc, g, g)
        h = torch.cat([
            self.ego(ego), self.creatures(creatures), self.shots(shots), self.bonuses(bonuses),
            self.local(local), self.glob(glob),
        ], dim=1)
        return self.mix(h)
