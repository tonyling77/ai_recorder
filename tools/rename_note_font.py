#!/usr/bin/env python3
"""Rename the note font so that it no longer carries a reserved font name.

Adobe's Source Han Sans has the reserved font name 'Source'. The subset in
this repository is a modified version, and OFL 1.1 section 3 forbids a
modified version from using that name. This script changes the primary
names only; the copyright notice keeps Adobe's wording, and the glyphs are
not touched.

Run it from the application directory:

    python3 tools/rename_note_font.py
"""

from pathlib import Path

from fontTools.ttLib import TTFont

FONTS = Path("components/app_hello/fonts")
SOURCE = FONTS / "SourceHanSansCN-GB.ttf"
TARGET = FONTS / "MosaicoNotesCN-GB.ttf"

# The primary names as they are presented to the user: family, subfamily,
# full name, PostScript name, unique id, typographic family and typographic
# subfamily. nameID 0 is the copyright notice and nameID 7 is the trademark;
# both keep Adobe's wording.
PRIMARY_IDS = (1, 2, 3, 4, 6, 16, 17)

REPLACEMENTS = (
    ("Source Han Sans CN", "Mosaico Notes CN"),
    ("SourceHanSansCN", "MosaicoNotesCN"),
)


def main() -> int:
    if not SOURCE.is_file():
        print(f"the font to rename is not there: {SOURCE}", flush=True)
        return 2
    font = TTFont(SOURCE)
    for record in font["name"].names:
        if record.nameID not in PRIMARY_IDS:
            continue
        try:
            value = record.toUnicode()
        except UnicodeDecodeError:
            continue
        for old, new in REPLACEMENTS:
            value = value.replace(old, new)
        record.string = value
    font.save(TARGET)
    print(f"wrote {TARGET}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
