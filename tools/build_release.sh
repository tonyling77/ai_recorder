#!/bin/sh
# Build the firmware that other people download and flash, and the bundle
# that is uploaded to the software page.
#
# The build carries no address of the inbox and no token: the board learns
# the address at runtime, so the image is the same for everybody. The
# values of a local .env are left out on purpose, and the checks at the end
# say that the image and the bundle carry nothing of the builder's.
#
# The address of the inbox is read when the build is configured, so the
# configure step has to run again: a build directory that was configured
# earlier would keep the address that was there then.
#
# The bundle is the System Update bundle: the application, the partition
# table and the resources of the interface in one file. It is what the
# software page takes, and it is what gives the user an install code.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

unset UPLOAD_ORIGIN
unset UPLOAD_TOKEN

touch components/app_recorder/CMakeLists.txt

python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project . build

python3 tools/check_release.py

# The files that are shared one by one. They are the same bytes that the
# bundle carries, so a user who flashes them by themselves ends up with the
# same board as a user who types the install code.
cp build/ai_recorder.bin \
   dist/ai_recorder-1.0.0.bin
cp build/partition_table/partition-table.bin \
   dist/partition-table.bin

# The bundle needs the pinned ESP-IDF and the host python that the ESP-Iris
# tools prepared. Both are resolved the way mosaico.py resolves them.
python3 tools/build_bundle.py

if [ -f build/ai_recorder-system-update.irisfw ]; then
    python3 tools/check_release.py \
        --bundle build/ai_recorder-system-update.irisfw
    cp build/ai_recorder-system-update.irisfw \
       dist/ai_recorder-1.0.0.irisfw

    # The manual package: the application, the partition table, the
    # resources of the interface, and the inbox for the developer.
    python3 tools/build_release.py
else
    echo
    echo "the bundle was not built; the software page needs it."
fi

echo
echo "the firmware is at build/ai_recorder.bin"
echo "the bundle is at build/ai_recorder-system-update.irisfw"
echo "it carries no address and no token: the board finds the inbox on its own"
