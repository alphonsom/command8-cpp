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
src/protocol.{hpp,cpp}   native MIDI protocol: decode (input) + encode (feedback)
src/surface.{hpp,cpp}    ALSA-seq I/O: device discovery, wake + keepalive, events
src/main.cpp             command8-monitor: PoC that prints decoded input events
```

`libcommand8` (protocol + surface) is DAW-agnostic. Host integrations (a Reaper
OSC bridge, a Mackie/HUI translator, a Bitwig backend, …) are meant to be built
*on top* of this library as separate front-ends — none are included yet.

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

- Value/event abstraction above the raw protocol (normalized 0..1 controls).
- Pluggable host back-ends (start by porting the Reaper profile as a front-end).
- systemd user service; hotplug (re-open on device arrival).
- Unit tests for `protocol` (decode/encode round-trips).

## License

GPL-3.0-or-later.
