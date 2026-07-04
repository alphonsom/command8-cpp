# command8-cpp

A native C++ userspace engine for the **Digidesign Command|8** control surface
on Linux and Windows. It is the DAW-agnostic core of the
[command8-linux](https://github.com/alphonsom/command8-linux) project (the
Python OSC bridge), reimplemented as a compiled library + daemon —
**without** any host-specific (Reaper/OSC) parts.

## Why userspace, not a kernel module

The Command|8 is a **class-compliant USB-MIDI device**. On Linux the only
kernel-side work is a small `snd-usb-audio` quirk that exposes its hidden MIDI
*input* port — that lives in `command8-linux` (DKMS) and is reused here
unchanged. (On Windows the class driver exposes the input as-is; no quirk is
needed.) Everything else (protocol translation, the wake/keepalive handshake,
LED/fader/meter/ring/LCD feedback) is ordinary userspace logic: it uses
floating point, is easy to debug, and a bug crashes one process instead of the
machine. So this engine is a normal compiled program that talks to the device
over ALSA (Linux) or RtMidi/WinMM (Windows).

## Layout

```
src/protocol.{hpp,cpp}    native MIDI protocol: decode (input) + encode (feedback)
src/surface.hpp           Surface interface: device discovery, wake + keepalive, events
src/midi_port.hpp         MidiPort interface: raw-bytes duplex port (MCU side)
src/alsa/                 ALSA-seq implementations of both (Linux)
src/rtmidi/               RtMidi implementations of both (Windows)
src/feedback.{hpp,cpp}    normalized (0..1) feedback: faders/meters/rings/LEDs/LCD
src/backend.hpp           Backend interface — host integrations subclass this
src/controller.{hpp,cpp}  wires Surface -> Backend, normalizes events
src/main.cpp              command8-monitor: demo Backend (loopback, no DAW)
src/reaper/               command8-reaper: Reaper OSC bridge (liblo)
src/mackie/               command8-mackie: Mackie Control (MCU) emulation
```

`libcommand8` is DAW-agnostic. A **Backend** receives normalized input
(`on_fader(strip, 0..1)`, `on_encoder(strip, ±1)`, `on_select`, …) and drives a
**Feedback** handle (`meter`, `ring_dot`, `select_led`, `lcd_channel`, …) that
hides the device bit-packing. Host integrations (a Reaper OSC bridge, a
Mackie/HUI translator, a Bitwig backend, …) are Backends built on top — the demo
in `main.cpp` is one (pure loopback: faders→meters, encoders→pan dot,
select/mute/solo→LEDs).

## Build (Linux)

Requires a C++17 compiler, CMake ≥ 3.16, and `libasound2-dev` (plus `liblo-dev`
for the Reaper bridge).

```sh
cmake -B build
cmake --build build
ctest --test-dir build                 # protocol decode/encode unit tests
./build/command8-monitor               # loopback demo (needs the device + quirk)
./build/command8-reaper                # Reaper OSC bridge (waits for the device)
./build/command8-mackie                # MCU bridge (needs snd-virmidi)
```

## Install / package

```sh
cmake --install build --prefix /usr/local     # binaries + systemd user unit + docs
```

Or build distributable packages (a `.deb` and a `.tar.gz`) with CPack:

```sh
cd build && cpack                              # -> command8-<ver>-Linux.deb / .tar.gz
sudo apt install ./command8-*-Linux.deb        # deps (libasound2, liblo) auto-resolved
```

Either way, `command8-reaper`'s `ExecStart` is rewritten to the real install
prefix (`/usr/bin` for the `.deb`, `/usr/local/bin` for a plain install), and the
systemd **user** unit lands in `<prefix>/lib/systemd/user/`.

Run `command8-reaper` as a **systemd user service** (self-heals on unplug/replug):

```sh
systemctl --user daemon-reload
systemctl --user enable --now command8-reaper
```

(Building from source without installing? Copy `systemd/command8-reaper.service.in`
to `~/.config/systemd/user/command8-reaper.service` and set `ExecStart` to your
`build/command8-reaper`.)

## Build (Windows)

Requires Visual Studio 2022+ (MSVC), CMake, and vcpkg (all bundled with a
Visual Studio install). Dependencies (RtMidi, liblo) come from the vcpkg
manifest automatically:

```bat
cmake -S . -B build -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build
ctest --test-dir build
build\command8-monitor.exe --list      # check the Command|8's ports are visible
build\command8-monitor.exe             # loopback demo
build\command8-reaper.exe              # Reaper OSC bridge (identical OSC setup)
build\command8-mackie.exe              # MCU bridge (see below)
```

(From a plain VS developer prompt, `%VCPKG_ROOT%` is
`%VSINSTALLDIR%VC\vcpkg`.)

The Command|8 needs no driver on Windows — the class driver exposes its MIDI
input and output (`Command|8`, plus `MIDIIN2/3` for the rear MIDI jacks). If
another app (a DAW) holds the port, close it first: WinMM ports are exclusive.

### Mackie bridge on Windows

Windows has no app-created virtual MIDI ports, so create a loopback pair once
with [Windows MIDI Services](https://aka.ms/midi) (or two loopMIDI cables and
`--mcu-recv`/`--mcu-send`):

```bat
midi loopback create --name-a "Command8 MCU A" --name-b "Command8 MCU B"
```

`command8-mackie` uses side **A** by default; point the DAW's Mackie Control
input *and* output at side **B**. The pair is crossed, so neither end hears its
own output.

## Status

Proof of concept: opens the device, performs the handshake, runs the keepalive,
decodes buttons/faders/encoders/fader-touch, and can drive all feedback (LEDs,
motor faders, meters, encoder rings, LCD). Protocol details and the
reverse-engineering evidence live in `command8-linux/docs/PROTOCOL.md`.

## Roadmap

- [x] Normalized value/event abstraction above the raw protocol.
- [x] Pluggable host Backend interface + Feedback handle.
- [x] Reaper Backend front-end (encoder modes, Flip, nav, LCD grid, actions,
      ring-on-knob-turn, Channel-Data fader flash) — hardware-validated.
- [x] systemd user service; hotplug (waits for the device, re-opens on replug).
- [x] Unit tests for `protocol` (decode/encode round-trips, via CTest).
- [x] Windows port: RtMidi Surface/MidiPort backends, vcpkg manifest, MSVC build.

## License

GPL-3.0-or-later.
