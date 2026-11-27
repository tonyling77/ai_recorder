# ESP-Mosaico ai_recorder

A meeting recorder for ESP-Mosaico. It records a meeting, sends the audio to
a cloud service or to an inbox on your own machine, and shows the minutes on
the board.

The same GSP 1.5.1 scene and the same portable C UI run on the PC and on the
board, so the interface can be checked in the simulator before it is put on
the device.

## What you need

- Python 3.10 or newer
- Git 2.28 or newer
- ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` for the `esp32s31` target.
  `bootstrap.sh` clones it for you if you do not have it.
- An ESP-Mosaico board

## Get the code and build it

```sh
git clone --recurse-submodules https://github.com/esp-mosaico/esp-ai-recorder.git
cd esp-ai-recorder
./bootstrap.sh
```

`bootstrap.sh` initializes the submodules, takes the pinned ESP-IDF, and
builds the firmware. The image is at `build/ai_recorder.bin`.

If you already have that ESP-IDF, point `IDF_PATH` at it and the script uses
it instead of cloning one.

To check the machine without building anything:

```sh
python mosaico.py doctor
```

## Put it on the board

```sh
python mosaico.py recover                            # a blank board, once
python mosaico.py iris system-update --project .     # a new application
python mosaico.py iris app-update --project .        # a change of code only
```

`system-update` is for a new application, or for a change of the partition
table or of the interface resources. `app-update` is for a change of code
alone, with the same partition table and the same resources.

To see what is there:

```sh
python mosaico.py iris list
python mosaico.py iris device-status
python mosaico.py iris logs
```

`iris` has more: `screenshot` saves the image of the screen, `memory` reads
the RAM and the stack high-water marks, `rpc` calls a service of the
application, `crash` keeps the crash evidence, and `run` holds a Gateway open
while you watch the device. `python mosaico.py iris --help` lists them all.

## The inbox

The board does not have the address of an inbox built into it. A developer
runs the inbox on their own machine, and the board finds it on the network,
so the same image works for everybody.

**1. Fill in the configuration.** Copy the example next to the program and put
your model key in it:

```sh
cp tools/inbox/config.example.yaml tools/inbox/inbox-config.yaml
$EDITOR tools/inbox/inbox-config.yaml
```

The inbox reads that file and nothing else. If you keep your model settings
somewhere else, point the inbox at it with `--config PATH`.

**2. Run the inbox.**

```sh
python3 tools/inbox/inbox.py --root inbox-data --host 0.0.0.0 --port 8765
```

The inbox announces itself as `_esp-inbox._tcp`, and the board looks for that.
A few seconds after the inbox starts, the settings page shows what was found:
swipe from the main page to the third page — 设置 — and look at the 收件箱
section. A tap on one of the addresses makes it the inbox the board talks to,
and the board keeps it across a restart and a firmware update.

**When the network does not allow that** — a guest Wi-Fi, a network with
client isolation — the address can be given over the link that carried the
update, with the board plugged into the laptop:

```sh
python3 tools/set_inbox.py --project .
```

The address that goes with the inbox is the name of the machine, so the
address of the laptop may change under the board.

### Who writes the summary

By default the inbox asks the model itself, through the `model` section of
the configuration. It can also ask an agent that runs on the same machine, if
one is installed: `hermes`, `workbuddy`, `opencode` and `muse` are looked up
as commands on the machine. An agent of your own is declared in the
`agents:` section of the configuration; see `tools/inbox/config.example.yaml`.

## The channels

The channel is which server the recordings go to. 私有 is the inbox on your
own machine; 小智 is the Xiaozhi cloud. One of them is on and the other is
off, and the board comes back on the channel it was on.

The inbox answers three paths: `POST /health` (is the link alive),
`POST /v1/slices` (one recording slice at a time, with the checksum and the
session headers), and `GET /v1/notes` (the minutes of every recording it
knows, in the card format the device reads). The transcript, the summary and
the card are written under `--root`, per device and per session.

A plain link does not need a wall clock, so the board keeps working on a
network without a route to the internet, where SNTP never answers.

## The release

`tools/build_release.sh` builds everything that is shared: the image, the
System Update bundle, and the package for a user who flashes the board by
themselves. The build carries nothing of the builder's: no address, no token,
no name. The check at the end of the script says so.

```sh
sh tools/build_release.sh
```

The address of the inbox is read when the build is configured, not when it
compiles. A changed `tools/inbox/inbox-config.yaml` needs the configure step
to run again:

```sh
rm -rf build   # then build again
```

## The layout

- `main`: device startup and dependency composition only.
- `components/app_hello`: the portable UI behavior and the controller state.
- `components/app_display`: the BSP-to-GSP display and the touch policy.
- `components/app_recorder`: the recording, the store on the flash, the upload.
- `components/app_onboard`: the network, the provisioning, the host services.
- `components/app_clock`: the clock of the board.
- `components/app_hello/host`: the desktop adapter, with its own CMake build,
  reusing the same controller and the same public header.
- `components/app_hello/ui/main.json`: the authored scene; the generated
  bindings and bundles stay in the build.
- `tools/inbox`: the inbox and the model services it talks to.
- `tools/quality`: the format rules and the project policy.
- `docs/`: the design of the inbox address and of the Xiaozhi agent.

The dependency direction is `main -> app_hello -> ESP-GSP` and
`main -> app_display -> BSP/ESP-GSP`. Neither application component depends
on `main` or on its sibling. The public headers live in `include/`; the
private state stays in the implementation files.

## Preview on the PC

```sh
python mosaico.py project sim --project . --interactive
```

The simulator runs the same C code that runs on the board, so a change to the
interface can be checked before it is flashed.

## The tests

```sh
make -C components/app_hello/host_tests test
make -C components/app_onboard/host_tests test
make -C components/app_recorder/host_tests test
cd tools/inbox && python3 -m unittest discover
```

## License

Apache-2.0. See `LICENSE`. The third-party components keep their own
licenses; see `THIRD_PARTY_NOTICES.md`.
