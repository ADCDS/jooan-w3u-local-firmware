#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Side by side: the design on the left, the build on the right, for the same file names.

    compose.py DESIGN_DIR BUILD_DIR OUT_DIR

The design's pictures come from wherever you render the design: its files stay private.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

design, build, out = (Path(a) for a in sys.argv[1:4])
out.mkdir(parents=True, exist_ok=True)
made = 0
for left_file in sorted(design.glob("*.png")):
    right_file = build / left_file.name
    if not right_file.exists():
        print(f"no build picture for {left_file.name}")
        continue
    left, right = Image.open(left_file).convert("RGB"), Image.open(right_file).convert("RGB")
    canvas = Image.new("RGB", (left.width + right.width + 24, max(left.height, right.height) + 28), "white")
    canvas.paste(left, (0, 28))
    canvas.paste(right, (left.width + 24, 28))
    draw = ImageDraw.Draw(canvas)
    draw.text((4, 8), "design", fill="black")
    draw.text((left.width + 28, 8), "build", fill="black")
    canvas.save(out / (left_file.stem + ".jpg"), quality=80)
    made += 1
print(f"{made} comparisons in {out}")
if not made:
    sys.exit("nothing compared: no picture has the same name in both directories")
