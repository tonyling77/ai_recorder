# app_clock

Owns the wall clock of the board. The component starts a plain lwIP SNTP client
and reports whether the system clock holds a usable date.

It exists because the RainMaker MQTT client used to carry the time sync. That
client is off: its endless TLS reconnect loop takes a hardware SHA context out
of the internal DMA pool on every TLS handshake, and the I2S microphone buffers
need that pool, so the recording failed with "Microphone unavailable". With the
client down the board lost its only clock source, the certificate bundle could
no longer validate a server certificate, and `recorder_stamp_now()` could not
produce a timestamp. This component replaces that clock source without putting
the MQTT client back.

## Responsibilities

- Start the SNTP client once the station has an address, and keep it running.
- Set the timezone to UTC+8 so a recorded stamp is in local time.
- Report the clock state to the callers that need it before they act.

It does not own the network, the recordings tree, the upload or the UI.

## Dependencies

`esp_netif` and `lwip` only. It depends on no other application component, so
`app_onboard`, `app_recorder` and `app_hello` may all use it without a cycle.

## Usage

```c
/* After the station has an address. The call does not block. */
app_clock_start();

if (!app_clock_valid()) {
    /* The handshake would fail with a TLS error that says nothing useful. */
}
```

`app_clock_start()` is idempotent and never blocks: the SNTP client runs on its
own task, so a slow DNS lookup or an unreachable server never holds up the
Wi-Fi join or the UI. The client retries the DNS lookup and the server on its
own, so a router that is not ready yet is not fatal. If the client itself
cannot be created, the task retries every `APP_CLOCK_RETRY_MS`.

`app_clock_valid()` is true once the clock reads a year of `APP_CLOCK_MIN_YEAR`
or later. Before the first sync the clock reads the epoch, so it is false.
Callers that open a TLS connection must check it first: the certificate bundle
validates the validity window of the server certificate against the system
clock.

`app_clock_log()` writes the current local time to the log. Use it to make the
clock state visible without having to record anything.

## Thread safety

`app_clock_valid()` and `app_clock_log()` read the system
clock and are safe to call from any task. `app_clock_start()` is safe to call
from an event handler; only the first call starts the client.
