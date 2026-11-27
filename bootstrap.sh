#!/bin/sh
# Prepare everything this repository needs and build the firmware.
#
# The pinned ESP-IDF is used when IDF_PATH points at it. Otherwise it is
# cloned into .idf/esp-idf inside this repository, so that the build does
# not depend on where the machine keeps its toolchains.
set -e

root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

IDF_REVISION=7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe

echo "==> initializing the submodules"
git submodule update --init --recursive

if [ -z "${IDF_PATH:-}" ]; then
    IDF_PATH="$root/.idf/esp-idf"
fi

# A checkout of ESP-IDF is either a clone or a worktree. A worktree keeps
# its .git as a file, so ask git instead of looking for a directory.
if ! git -C "$IDF_PATH" rev-parse HEAD >/dev/null 2>&1; then
    if [ -e "$IDF_PATH" ] && [ -n "$(ls -A "$IDF_PATH" 2>/dev/null)" ]; then
        echo "$IDF_PATH is not empty and is not a checkout of ESP-IDF." >&2
        echo "Point IDF_PATH at a checkout, or remove the directory." >&2
        exit 1
    fi
    echo "==> cloning ESP-IDF into $IDF_PATH"
    git clone --filter=blob:none https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi

current=$(git -C "$IDF_PATH" rev-parse HEAD)
if [ "$current" != "$IDF_REVISION" ]; then
    echo "==> taking ESP-IDF $IDF_REVISION"
    git -C "$IDF_PATH" fetch --filter=blob:none origin
    git -C "$IDF_PATH" checkout "$IDF_REVISION"
fi
export IDF_PATH

echo "==> building"
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project . build

echo
echo "the firmware is at build/ai_recorder.bin"
