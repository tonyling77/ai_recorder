# The address of the inbox

## The problem

A developer pulls the repository, flashes the board and runs the inbox on
their own machine. The address of that machine is not known when the firmware
is built, and the address of a laptop changes when the lease runs out.

So the address cannot be a build input. It has to be learned.

## The shape of the answer

**The address is learned at runtime and kept in NVS.** The firmware is the
same for everybody: no token, no host, no `.env`. The learned address
survives a restart and a firmware update, because NVS is not part of the
application partition.

**The learned address is a name, not an address.** A name that ends in
`.local` is resolved by the board with a multicast mDNS query (one-shot,
RFC 6762 §5.1), which lwIP already does for every name that ends that way.
The name follows the machine, so the address of the machine may change under
it. The inbox prints the name it has, and the SRV record of a published
service gives the host name rather than the address, so the origin the board
stores is a name too.

## The three ways the address is learned

| way | what the developer does | what the board stores |
| --- | --- | --- |
| discovery (mDNS) | runs the inbox | `http://<host>.local:<port>` |
| the host, over the link to the laptop | points the board at the inbox | what the host sent |
| the build | fills in `UPLOAD_ORIGIN` | nothing: it is the fallback |

The learned address comes first. The build-time origin is the fallback, so a
board that was built for a private inbox behind a tunnel keeps working.

When nothing has been learned and there is no build-time origin either, the
default is `http://inbox.local:8765`: a name that a machine called "inbox"
answers to, with no setup at all.

## The rules that were decided

**When multicast does not work.** Some networks block it: a guest Wi-Fi, a
network with client isolation, a company network. The fallback chain is the
learned address, then the build-time origin, and the board says plainly that
it has no inbox rather than failing quietly.

**When there is more than one inbox.** The one that is in use stays in use.
The others are shown, and the user picks. A candidate that was never
confirmed is never adopted silently: the board says that it found an inbox
and waits for the user to confirm it.

**Whether a stranger on the network may point the board at their own
server.** Not for now. The threat is accepted for a development tool on a
local network. Tightening it later means requiring the token in the
published record to match the one the board holds, which costs the developer
one setup step.

## What the host sends

The host tells the board where the inbox is over the link that carried the
update:

```sh
python mosaico.py iris rpc 0x1002 1 \
    --payload "http://your-laptop.local:8765" \
    --project .
```

The payload is the origin in text, and optionally the name of the machine on
the next line. The reply is the origin that is now in use, or a line that
starts with `error:`.

The service id `0x1002` is in the range the protocol reserves for the
application: `0x1001` is the pointer profile and `0x7fff` is recovery. The
handler lives in `components/app_onboard/inbox_config_rpc.c`, because
onboarding is the responsibility that owns how the device is set up from the
outside.

## The parts

- `components/app_recorder/recorder_inbox.c` owns the learned address: the
  copy in memory, the copy in NVS, and the rules for what a usable address
  is.
- `components/app_recorder/recorder_upload.c` builds the request URL. It
  takes the learned address when there is one, and the build-time origin
  otherwise.
- `components/app_onboard/inbox_config_rpc.c` is the door the host knocks on.
- `tools/inbox/inbox.py` is the inbox. It prints the origin to use and, in
  the next step, publishes itself as `_esp-inbox._tcp`.

## The steps, and where they are

1. **The learned address** — done. The address is learned, kept in NVS, and
   the host can point the board at one.
2. **Discovery** — done. The board browses for `_esp-inbox._tcp` and the
   inbox publishes itself. The IDF `mdns` component is already in the
   dependency tree through ESP-Iris, and its browse API is already enabled.
3. **Visible and changeable** — done. The settings page, the third one,
   carries the channel, the inbox and the Xiaozhi binding. A found inbox is
   never adopted silently: the page lists it and a tap on it is the user
   saying that this is the one to use.
4. **A friendlier command** — done. `tools/set_inbox.py` points the board at
   the inbox over the link that carried the update, for the networks where
   the announcement does not get through.

## What the screen looks like

```
设置
通道
[ 私有 ] [ 小智 ]
当前通道：私有

收件箱
your-laptop.local:8765
[ another-laptop.local:8765 ]
点一个用它

小智
------                (the activation code)
已绑定
```

The Chinese is written by the application, not by the scene: the scene
carries only Latin fonts, so Chinese that is written into the scene comes
up as question marks. Chinese that the application writes is drawn with the
font of the application, which has the glyphs.
