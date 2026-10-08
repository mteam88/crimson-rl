"""Trains the Crimsonland policy with PufferLib's PyTorch backend.

Usage: .venv/bin/python puffer/train.py [train|eval] [pufferlib options, e.g. --train.total-timesteps 1e9]
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pufferlib.models
import pufferlib.pufferl

from policy import CrimsonEncoder

pufferlib.models.CrimsonEncoder = CrimsonEncoder

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "train"
    sys.argv = [sys.argv[0], mode, "crimson", "--slowly", *sys.argv[2:]]
    pufferlib.pufferl.main()
