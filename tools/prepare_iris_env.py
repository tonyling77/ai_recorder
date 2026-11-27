#!/usr/bin/env python3
"""Prepare the ESP-Iris host environment and name its Python.

The System Update bundle is built by the ESP-Iris tools, and the target that
builds it reads ESP_IRIS_PYTHON **when CMake configures** (see
`mosaico-tools/cmake/system_update.cmake`). The environment therefore has to
be ready and named before the build starts, not after it.

This script prepares the environment the way the product does and prints the
path of its Python:

    export ESP_IRIS_PYTHON=$(python3 tools/prepare_iris_env.py)

Run it from the repository root.
"""

from __future__ import annotations

import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def utils_root() -> Path:
    """Where the tools of the workspace live.

    The application is the repository root in the standalone repository and a
    directory under `projects/` in the workspace, so the place is looked for
    from the application upwards.
    """
    for candidate in (PROJECT, *PROJECT.parents):
        tools = candidate / "submodule" / "esp-mosaico-utils" / "mosaico-tools"
        if tools.is_dir():
            return tools
    raise SystemExit("the tools of the workspace are not there")


def main() -> int:
    tools = utils_root()
    sys.path.insert(0, str(tools / "tools"))
    from mosaico_cli.gateway import ensure_iris_tools
    from mosaico_cli.runtime import RunContext
    from mosaico_cli.workspace import load_workspace

    workspace = load_workspace(tools, start=PROJECT)
    context = RunContext(workspace, "release", False, False)
    python, _ = ensure_iris_tools(context)
    print(python)
    return 0


if __name__ == "__main__":
    sys.exit(main())
