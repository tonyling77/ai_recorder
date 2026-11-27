#!/usr/bin/env python3
"""Check that a built firmware carries nothing of the builder's.

A firmware that other people download and flash must not carry the address
of the inbox of the person who built it, their token, or anything else that
is theirs. The build is where such a thing enters, so the check looks at
the image that was built.

    python3 tools/check_release.py

The check looks for an address with a number in it, for a name that is not
one of the generic ones the project uses, and for a token of the shape the
inbox uses. It also looks for the values of the local .env, if there is
one, because those are exactly what must not be in the image.
"""

from __future__ import annotations

import argparse
import re
import sys
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]

# The names the project itself uses. They are generic on purpose: a board
# that was flashed by anybody can reach them. The Xiaozhi cloud is a
# product endpoint, not the builder's business.
GENERIC_HOSTS = {"inbox.local", "localhost", "127.0.0.1", "api.tenclass.net"}

# An address with a number in it: the address of a machine on a network.
ADDRESS = re.compile(rb"https?://(\d{1,3}(?:\.\d{1,3}){3})")
# A name that is used as a host.
HOST = re.compile(rb"https?://([A-Za-z0-9][A-Za-z0-9._-]*\.[A-Za-z]{2,})")
# The token the inbox hands out is a long hex value.
TOKEN = re.compile(rb"\b[0-9a-f]{64}\b")
# The home directory of the machine that built the image. A log line that
# carries __FILE__ puts it there, and it says who built it and where.
HOME = re.compile(rb"/Users/[A-Za-z0-9._-]+/|/home/[A-Za-z0-9._-]+/|C:\\\\Users\\\\[A-Za-z0-9._-]+")


def is_a_token(value: bytes) -> bool:
    """Whether the run of hex is a token and not a constant of a library.

    The crypto library carries constants that are runs of the same digit,
    and those are in every image."""
    return len(set(value)) >= 8


def home_paths(image: bytes) -> list[bytes]:
    """The home directories that the image carries.

    The directory of the machine that built it is the one that matters. The
    other shapes are the ones a build machine of another kind would leave
    behind, so that an image built somewhere else does not pass by accident.
    """
    found = []
    for match in HOME.finditer(image):
        value = match.group(0)
        if str(Path.home()).encode() in value:
            found.append(value)
            continue
        if value.startswith(b"/Users/") or value.startswith(b"/home/") or value.startswith(b"C:"):
            found.append(value)
    return found


def strings(image: bytes) -> list[bytes]:
    """The printable runs of the image."""
    return re.findall(rb"[\x20-\x7e]{6,}", image)


def env_values() -> list[tuple[str, str]]:
    """The values of the local .env, which must not be in the image."""
    path = PROJECT / ".env"
    if not path.is_file():
        return []
    values = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 8:
            values.append((key.strip(), value))
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default=str(PROJECT / "build" / "ai_recorder.bin"),
                        help="the firmware image to check")
    parser.add_argument("--bundle", default="",
                        help="the .irisfw bundle to check; every part of it is checked")
    arguments = parser.parse_args()

    if arguments.bundle:
        return check_bundle(Path(arguments.bundle))

    image_path = Path(arguments.image)
    if not image_path.is_file():
        print(f"the image {image_path} does not exist", file=sys.stderr)
        return 2
    image = image_path.read_bytes()
    found: list[str] = []

    for match in ADDRESS.finditer(image):
        found.append(f"an address with a number in it: {match.group(0).decode('ascii', 'replace')}")
    for match in HOST.finditer(image):
        host = match.group(1).decode("ascii", "replace").lower()
        if host not in GENERIC_HOSTS and not host.endswith(".invalid"):
            found.append(f"a host that is not generic: {match.group(0).decode('ascii', 'replace')}")
    for match in TOKEN.finditer(image):
        if is_a_token(match.group(0)):
            found.append("a token of the shape the inbox uses")
    for key, value in env_values():
        if value.encode("utf-8") in image:
            found.append(f"the {key} of the local .env")
    for value in home_paths(image):
        found.append(f"the home directory of a build machine: {value.decode('ascii', 'replace')}")

    # The same thing, read the way a person would read it out of the image.
    for run in strings(image):
        for match in ADDRESS.finditer(run):
            found.append(f"an address with a number in it: {match.group(0).decode('ascii', 'replace')}")
        for match in HOST.finditer(run):
            host = match.group(1).decode("ascii", "replace").lower()
            if host not in GENERIC_HOSTS and not host.endswith(".invalid"):
                found.append(f"a host that is not generic: {match.group(0).decode('ascii', 'replace')}")
        for match in TOKEN.finditer(run):
            if is_a_token(match.group(0)):
                found.append("a token of the shape the inbox uses")

    unique = sorted(set(found))
    if unique:
        print(f"{image_path} carries {len(unique)} thing(s) of the builder:")
        for item in unique:
            print(f"  - {item}")
        return 1
    print(f"{image_path} carries nothing of the builder's")
    return 0


def check_bundle(bundle_path: Path) -> int:
    """Check every part of the bundle.

    The bundle is what other people download, and it holds more than the
    application: the partition table and the data with the fonts and the
    pictures of the interface. A part of it that carries the address of the
    inbox of the builder would be just as bad as one in the application.
    """
    if not bundle_path.is_file():
        print(f"the bundle {bundle_path} does not exist", file=sys.stderr)
        return 2
    found: list[str] = []
    with zipfile.ZipFile(bundle_path) as archive:
        names = [name for name in archive.namelist() if not name.endswith(".json")]
        if not names:
            print(f"the bundle {bundle_path} holds nothing to check", file=sys.stderr)
            return 2
        for name in names:
            part = archive.read(name)
            for match in ADDRESS.finditer(part):
                found.append(f"{name}: an address with a number in it: "
                             f"{match.group(0).decode('ascii', 'replace')}")
            for match in HOST.finditer(part):
                host = match.group(1).decode("ascii", "replace").lower()
                if host not in GENERIC_HOSTS and not host.endswith(".invalid"):
                    found.append(f"{name}: a host that is not generic: "
                                 f"{match.group(0).decode('ascii', 'replace')}")
            for match in TOKEN.finditer(part):
                if is_a_token(match.group(0)):
                    found.append(f"{name}: a token of the shape the inbox uses")
            for key, value in env_values():
                if value.encode("utf-8") in part:
                    found.append(f"{name}: the {key} of the local .env")
            for value in home_paths(part):
                found.append(f"{name}: the home directory of a build machine: "
                             f"{value.decode('ascii', 'replace')}")

    unique = sorted(set(found))
    if unique:
        print(f"{bundle_path} carries {len(unique)} thing(s) of the builder:")
        for item in unique:
            print(f"  - {item}")
        return 1
    print(f"{bundle_path} carries nothing of the builder's")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
