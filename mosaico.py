#!/usr/bin/env python3
"""Launch the product CLI pinned by this repository."""
from pathlib import Path
import sys

if sys.version_info < (3, 10):
    print("mosaico: this repository requires Python 3.10 or newer", file=sys.stderr)
    raise SystemExit(3)

TOOLS_ROOT = Path(__file__).resolve().parent / "submodule/esp-mosaico-utils/mosaico-tools"
if not (TOOLS_ROOT / "tools/mosaico_cli/cli.py").is_file():
    print("mosaico: initialize the tools with "
          "'git submodule update --init --recursive'", file=sys.stderr)
    raise SystemExit(3)
sys.path.insert(0, str(TOOLS_ROOT / "tools"))
from mosaico_cli.cli import main

if __name__ == "__main__":
    raise SystemExit(main(tool_root=TOOLS_ROOT))
