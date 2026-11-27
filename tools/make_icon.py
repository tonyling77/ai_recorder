#!/usr/bin/env python3
"""Draw the icon of the application.

The listing of an application shows its icon, and the board shows it as the
picture of the application. A drawing of a microphone is enough: it says what
the application is without a photograph of a board that every reader does not
have.

    python3 tools/make_icon.py

It writes docs/publish/icon.png: 64 by 64 pixels, with an alpha channel, the
shape the platform asks for.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

PROJECT = Path(__file__).resolve().parents[1]
TARGET = PROJECT / "docs" / "publish" / "icon.png"
SIZE = 64
SCALE = 8

BACKGROUND = (27, 36, 48, 255)
MICROPHONE = (255, 255, 255, 255)
ACCENT = (255, 176, 64, 255)


def draw() -> Image.Image:
    canvas = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    paint = ImageDraw.Draw(canvas)

    paint.rounded_rectangle((0, 0, SIZE * SCALE - 1, SIZE * SCALE - 1),
                            radius=12 * SCALE, fill=BACKGROUND)

    # The capsule of the microphone.
    paint.rounded_rectangle((22 * SCALE, 12 * SCALE, 42 * SCALE, 38 * SCALE),
                            radius=10 * SCALE, fill=MICROPHONE)
    # The holder under it.
    paint.arc((16 * SCALE, 20 * SCALE, 48 * SCALE, 48 * SCALE),
              start=0, end=180, fill=ACCENT, width=3 * SCALE)
    # The stem and the foot.
    paint.rectangle((30 * SCALE, 40 * SCALE, 34 * SCALE, 50 * SCALE), fill=ACCENT)
    paint.rounded_rectangle((22 * SCALE, 49 * SCALE, 42 * SCALE, 53 * SCALE),
                            radius=2 * SCALE, fill=ACCENT)

    return canvas.resize((SIZE, SIZE), Image.LANCZOS)


def main() -> int:
    icon = draw()
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    icon.save(TARGET, format="PNG", optimize=True)
    print(f"the icon is at {TARGET} ({TARGET.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
