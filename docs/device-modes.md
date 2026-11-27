# Device modes

[简体中文](device-modes_CN.md)

**Vibe Mode** is ESP-Mosaico's built-in application installation and device
maintenance mode. It runs ESP-Iris and provides application updates and Wi-Fi
setup. **ROM Download Mode** is the chip's own firmware download program; it
does not run ESP-Iris.

| Mode | What runs | When to use it |
| --- | --- | --- |
| Normal application | Your application; ESP-Iris is available when integrated and started | Run, debug and verify your application |
| Vibe Mode | The retained firmware in the flash `vibe_mode` partition, including ESP-Iris | Install or update applications, maintain the device |
| ROM Download Mode | The chip's built-in download program, without ESP-Iris | Provision a blank device, or restore base firmware that no longer works |

An offline device or a failed handshake does not prove ROM mode or damaged
firmware. Check the connection and the device state first.

## Which command to use

| Situation | Command |
| --- | --- |
| A blank board, or one whose base firmware is missing | `python mosaico.py recover` |
| A new application, or a change of the partition table or of the interface resources | `python mosaico.py iris system-update --project .` |
| A change of the code alone, with the same partition table and the same resources | `python mosaico.py iris app-update --project .` |
| The application is running and you want to watch the device | `python mosaico.py iris run --project .` |

`recover` writes the base firmware and then verifies Vibe Mode. It can start
from a connected device. **Manual ROM entry is only needed when the workflow
asks for it** — the command says so, and you enter ROM Download Mode as
described below.

`system-update` is the one to use for a new application. `app-update` is
only safe when the partition table and the interface resources are unchanged.

## Physical entry

Two different buttons, two different startup paths.

- **ROM Download Mode — Boot button (GPIO61):** only when `recover` asks for
  it. Turn the power off, **hold Boot** (the button to the left of the
  USB-C port), turn the power on while holding it, and release Boot after the
  chip has entered ROM Download Mode. Then tell the tool that the physical
  step is done; it verifies the ROM connection and resumes `recover`. Vibe
  Mode and ESP-Iris do not run in this mode.
- **Vibe Mode — AI button (GPIO7):** with a working bootloader and retained
  firmware, hold AI while powering on and release it after Vibe Mode starts.
  The bootloader selects the `vibe_mode` partition for that boot without
  changing the OTA selection. After a one-boot entry with the AI button
  released, a restart follows the existing OTA selection, if that is valid.

A board that has never been provisioned has no retained firmware, so the AI
button does nothing on it — that is the case `recover` is for.

## After the update

An update that reports success has not finished the job. Before you trust it:

1. Read the device identity and state:
   ```sh
   python mosaico.py iris device-status
   ```
2. Confirm the intended application is running and does what it is supposed
   to do. `system-update` and `app-update` select and boot the installed
   application; a successful write is not the same as a working device.
3. Keep the evidence: the output of the command, and, when something went
   wrong, `python mosaico.py iris crash` preserves the crash evidence.

## If the update does not go through

- A board that does not appear at all: check the USB cable and the port.
  The product uses USB for both power and the download link.
- A board that appears but refuses the update: `python mosaico.py iris
  status` shows what the Gateway knows about the device and whether another
  client holds it.
- A board whose base firmware is gone: `python mosaico.py recover`, and enter
  ROM Download Mode by hand if it asks.
- After the base firmware is back, verify that Vibe Mode comes up before
  installing the application.
