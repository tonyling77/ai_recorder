#!/usr/bin/env python3
"""Draw the cover of the application.

The listing shows the cover before anything else. A photograph of a board
would be best, but every reader does not have one, so the cover says what the
application is: the icon, the name, and the one sentence that tells whether
this is the application the reader wants.

    python3 tools/make_cover.py

It writes docs/publish/cover.png. The Chinese is drawn with the font the board
carries, so the cover and the board look the same.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

PROJECT = Path(__file__).resolve().parents[1]
TARGET = PROJECT / "docs" / "publish" / "cover.png"
ICON = PROJECT / "docs" / "publish" / "icon.png"
CHINESE = PROJECT / "components" / "app_hello" / "fonts" / "MosaicoNotesCN-GB.ttf"
LATIN = PROJECT / "components" / "app_hello" / "ui" / "fonts" / "DejaVuSans.ttf"

WIDTH, HEIGHT = 1080, 720
BACKGROUND = (16, 23, 32, 255)
TITLE = (255, 255, 255, 255)
BODY = (206, 214, 224, 255)
ACCENT = (255, 176, 64, 255)

TITLE_TEXT = "AI 录音笔"
SUBTITLE_LINES = ["按一下就录音，会后自动变成带结论和待办的会议纪要，", "纪要回到板子上，不用把文件导出来看。"]
FOOTER_LATIN = "ESP-Mosaico"
FOOTER_CHINESE = "私有通道 / 小智通道"


def font(path: Path, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(path), size)


def draw() -> Image.Image:
    canvas = Image.new("RGBA", (WIDTH, HEIGHT), BACKGROUND)
    paint = ImageDraw.Draw(canvas)

    icon = Image.open(ICON).convert("RGBA").resize((140, 140), Image.LANCZOS)
    canvas.alpha_composite(icon, (90, 92))

    paint.text((90, 285), TITLE_TEXT, font=font(CHINESE, 92), fill=TITLE)
    paint.rectangle((90, 410, 90 + 120, 410 + 8), fill=ACCENT)
    for index, line in enumerate(SUBTITLE_LINES):
        paint.text((90, 450 + index * 52), line, font=font(CHINESE, 38), fill=BODY)

    latin = font(LATIN, 30)
    chinese = font(CHINESE, 30)
    x = 90
    paint.text((x, 620), FOOTER_LATIN, font=latin, fill=(150, 160, 172, 255))
    x += paint.textlength(FOOTER_LATIN, font=latin) + 18
    paint.text((x, 620), "·", font=latin, fill=ACCENT)
    x += paint.textlength("·", font=latin) + 18
    paint.text((x, 620), FOOTER_CHINESE, font=chinese, fill=(150, 160, 172, 255))

    return canvas.convert("RGB")


def main() -> int:
    cover = draw()
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    cover.save(TARGET, format="PNG", optimize=True)
    print(f"the cover is at {TARGET} ({TARGET.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
