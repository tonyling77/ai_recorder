# AI Recorder for the ESP-Mosaico board

A recorder that takes a meeting, sends the audio to a machine on the same
network, and shows the minutes of the meeting on the board.

The firmware in `ai_recorder-1.0.0.bin` is the one to flash. It carries
nothing of the person who built it: no address, no token, no name. Every
developer flashes the same image and points it at their own machine.

## What it does

- **Record** — the button on the recorder page. The audio goes to the flash
  of the board, and the recording appears in the list.
- **Upload** — the audio goes to the inbox that runs on the machine of the
  developer. The machine turns it into text and a summary.
- **Minutes** — the second page shows the minutes of the meetings that the
  machine sent back. They survive a restart.
- **Two channels** — the private inbox of the developer, or the Xiaozhi
  cloud. The choice is on the third page and the board remembers it.

## What you need

- The board, with the ESP-Mosaico firmware on it.
- A machine on the same network as the board.
- Python 3.10 or later on that machine.

## The inbox on your machine

The inbox receives the recordings and turns them into minutes. It also
answers the board's health check, so the board knows it is there.

```sh

python3 tools/inbox/inbox.py --root inbox-data --port 8765
```

The inbox prints the address the board should use:

```
the inbox is at http://<your-machine>.local:8765
```

## Pointing the board at your inbox

The board finds the inbox on its own: the inbox announces itself on the
network, and the board asks before it uses one. The address it finds is a
**name**, not an address, so it keeps working when the address of your
machine changes.

If the network does not allow that, give the address over the link that
carries the update:

```sh
python3 mosaico.py iris rpc 0x1002 1 \
    --payload "http://<your-machine>.local:8765" \
    --project .
```

The board keeps the address, so it survives a restart and an update.

## Flashing

```sh
python3 mosaico.py iris system-update --project .
```

The build directory of the project holds the image that was flashed. To
flash the image in this folder instead, use the update command of the
product tooling with that image.

## Checking it works

The board can be asked to do what the buttons do, over the same link, so
the whole path can be checked without a person in front of it:

```sh
python3 tools/board_check.py --device-id <id>
```

It records, stops, and then checks that the recording is on the flash,
that the board did not fault, and that the minutes are there.

## The parts

| Where | What |
| --- | --- |
| `components/app_hello` | the interface and the flow of the recording |
| `components/app_recorder` | the recording, the store on the flash, the upload |
| `components/app_onboard` | the network, the provisioning, the services for the host |
| `components/app_clock` | the clock of the board |
| `tools/inbox` | the inbox on the machine of the developer |
| `tools/board_check.py` | the check of the board from the host |
| `tools/sim_check.py` | the check of the interface in the simulator |
| `docs/` | the design of the inbox address and of the Xiaozhi agent |

## Notes for the developer

- The board finds the inbox by itself. The build carries no address, and
  the check `tools/check_release.py` says so.
- The stack of the task that writes the recording is the size the write
  path needs. It is written down where it is set, with the reason.
- The logs of the board stop after about fifteen seconds, so anything that
  has to be seen has to be on the screen or in a line that is written
  before the log is cut.
