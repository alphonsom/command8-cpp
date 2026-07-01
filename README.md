# command8-cpp

A native C++ userspace engine for the **Digidesign Command|8** control surface on
Linux. It is the DAW-agnostic core of the [command8-linux](https://github.com/alphonsom/command8-linux)
project (the Python OSC bridge), reimplemented as a compiled library + daemon —
**without** any host-specific (Reaper/OSC) parts.

## Why userspace, not a kernel module

The Command|8 is a **class-compliant USB-MIDI device**. The only kernel-side work
is a small `snd-usb-audio` quirk that exposes its hidden MIDI *input* port — that
lives in `command8-linux` (DKMS) and is reused here unchanged. Everything else
(protocol translation, the wake/keepalive handshake, LED/fader/meter/ring/LCD
feedback) is ordinary userspace logic: it uses floating point, is easy to debug,
and a bug crashes one process instead of the machine. So this engine is a normal
compiled program that talks to the device over ALSA.

## Layout

```
src/protocol.{hpp,cpp}    native MIDI protocol: decode (input) + encode (feedback)
src/surface.{hpp,cpp}     ALSA-seq I/O: device discovery, wake + keepalive, events
src/feedback.{hpp,cpp}    normalized (0..1) feedback: faders/meters/rings/LEDs/LCD
src/backend.hpp           Backend interface — host integrations subclass this
src/controller.{hpp,cpp}  wires Surface -> Backend, normalizes events
src/main.cpp              command8-monitor: demo Backend (loopback, no DAW)
```

`libcommand8` is DAW-agnostic. A **Backend** receives normalized input
(`on_fader(strip, 0..1)`, `on_encoder(strip, ±1)`, `on_select`, …) and drives a
**Feedback** handle (`meter`, `ring_dot`, `select_led`, `lcd_channel`, …) that
hides the device bit-packing. Host integrations (a Reaper OSC bridge, a
Mackie/HUI translator, a Bitwig backend, …) are Backends built on top — the demo
in `main.cpp` is one (pure loopback: faders→meters, encoders→pan dot,
select/mute/solo→LEDs).

## Build

Requires a C++17 compiler, CMake ≥ 3.16, and `libasound2-dev`.

```sh
cmake -B build
cmake --build build
./build/command8-monitor      # needs the device + the snd-usb-audio quirk loaded
```

## Status

Proof of concept: opens the device, performs the handshake, runs the keepalive,
decodes buttons/faders/encoders/fader-touch, and can drive all feedback (LEDs,
motor faders, meters, encoder rings, LCD). Protocol details and the
reverse-engineering evidence live in `command8-linux/docs/PROTOCOL.md`.

## Roadmap

- [x] Normalized value/event abstraction above the raw protocol.
- [x] Pluggable host Backend interface + Feedback handle.
- [ ] Port the Reaper profile as a Backend front-end (encoder modes, LCD grid, …).
- [ ] systemd user service; hotplug (re-open on device arrival).
- [ ] Unit tests for `protocol` (decode/encode round-trips).

## License

GPL-3.0-or-later.
