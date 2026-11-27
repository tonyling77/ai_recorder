#!/usr/bin/env python3
"""Build the System Update bundle of this application.

The bundle is what the software page takes: the application, the partition
table and the resources of the interface in one file. It is built by the
`system-update-bundle` target of the project, which needs two prepared host
environments:

- the python of the pinned ESP-IDF, with the environment that IDF exports;
- the python that the ESP-Iris tools prepared, because the bundle builder
  uses their dependencies.

Both are resolved the way mosaico.py resolves them, so this script does not
guess paths. Run it through tools/build_release.sh, which builds the image
first.

    python3 tools/build_bundle.py
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parent
UTILS = WORKSPACE / "submodule" / "esp-mosaico-utils"
PINNED_IDF = Path(os.environ.get("IDF_PATH", WORKSPACE / ".idf" / "esp-idf")).expanduser()


def iris_python() -> Path | None:
    """The python that the ESP-Iris host tools prepared."""
    sys.path.insert(0, str(UTILS / "mosaico-tools" / "tools"))
    from mosaico_cli.gateway import iris_environment_root
    from mosaico_cli.host import virtual_environment_python

    python = virtual_environment_python(iris_environment_root(UTILS / "ESP-Iris"))
    return python if python.is_file() else None


def main() -> int:
    sys.path.insert(0, str(UTILS / "mosaico-tools" / "tools"))
    from mosaico_cli.host import prepare_idf_environment

    iris = iris_python()
    if iris is None:
        print("the ESP-Iris host environment is not prepared", file=sys.stderr)
        return 2

    idf_path = Path(os.environ.get("IDF_PATH", str(PINNED_IDF))).expanduser()
    prepared = prepare_idf_environment(idf_path)

    environment = dict(prepared.values)
    environment["ESP_IRIS_PYTHON"] = str(iris)
    environment["IDF_PATH"] = str(idf_path)

    command = [
        str(prepared.python),
        str(prepared.idf_py),
        "-C", str(PROJECT),
        "-B", str(PROJECT / "build"),
        "system-update-bundle",
    ]
    print("bundle:", " ".join(command))
    result = subprocess.run(command, cwd=WORKSPACE, env=environment)
    if result.returncode != 0:
        print("the bundle was not built", file=sys.stderr)
        return result.returncode

    bundle = PROJECT / "build" / f"{PROJECT.name}-system-update.irisfw"
    if not bundle.is_file():
        print(f"the bundle {bundle} was not produced", file=sys.stderr)
        return 2
    print(f"the bundle is at {bundle} ({bundle.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
