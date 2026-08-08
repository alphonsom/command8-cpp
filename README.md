# command8-cpp

A native C++ userspace engine for the **Digidesign Command|8** control surface
on Linux, Windows and macOS: a DAW-agnostic core library + bridges for Reaper (OSC)
and any Mackie-Control-capable DAW (Bitwig, …). The protocol documentation ([docs/PROTOCOL.md](docs/PROTOCOL.md)),
the Linux kernel quirk ([quirk/](quirk/)) and the Reaper OSC pattern
([reaper/](reaper/)) are all included here.

The device's MIDIStreaming *input* descriptor is malformed, so a standard class
parser does not create an input port — and each platform needs a different way
around that:

| | getting the input | MCU bridge needs |
|---|---|---|
| **Linux** | `snd-usb-audio` quirk ([quirk/](quirk/)) | `snd-virmidi` |
| **Windows** | Digidesign/Avid's own driver | a loopback pair |
| **macOS** | claim the USB interface directly (libusb) | nothing |

macOS is the odd one out in both columns. CoreMIDI has no quirk mechanism, so
the device's own ports enumerate but stay inert; the backend bypasses CoreMIDI
on the device side and speaks USB-MIDI packets over libusb instead. In exchange,
macOS *can* create MIDI endpoints from an application, so the MCU bridge
publishes its own virtual pair and needs no loopback utility.

Everything else (protocol translation, the wake/keepalive handshake,
LED/fader/meter/ring/LCD feedback) is ordinary userspace logic: So this engine is a normal compiled program that talks to the device
over ALSA (Linux), RtMidi/WinMM (Windows) or libusb (macOS), giving full access to the surface controls and feedback, but with some buttons (EQ, Dynamics) not reproducing the exact function they have in Pro Tools.

## Layout

```
src/protocol.{hpp,cpp}    native MIDI protocol: decode (input) + encode (feedback)
src/surface.hpp           Surface interface: device discovery, wake + keepalive, events
src/midi_port.hpp         MidiPort interface: raw-bytes duplex port (MCU side)
src/alsa/                 ALSA-seq implementations of both (Linux)
src/rtmidi/               RtMidi implementations of both (Windows)
src/macos/                libusb Surface + virtual-CoreMIDI MidiPort (macOS)
src/feedback.{hpp,cpp}    normalized (0..1) feedback: faders/meters/rings/LEDs/LCD
src/backend.hpp           Backend interface — host integrations subclass this
src/controller.{hpp,cpp}  wires Surface -> Backend, normalizes events
src/main.cpp              command8-monitor: demo Backend (loopback, no DAW)
src/reaper/               command8-reaper: Reaper OSC bridge (liblo)
src/mackie/               command8-mackie: Mackie Control (MCU) emulation
docs/PROTOCOL.md          protocol reverse-engineering evidence (+ raw captures)
quirk/                    Linux snd-usb-audio quirk patch (exposes the MIDI input)
reaper/Command8.ReaperOSC Reaper OSC pattern file for command8-reaper
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

## Reaper setup (all platforms)

In Reaper: Preferences → Control/OSC/web → Add → **OSC**. Set the pattern
config to [reaper/Command8.ReaperOSC](reaper/Command8.ReaperOSC) (installed
packages put it in `<prefix>/share/command8/`), device receives on **8000**,
sends to **9000** — `command8-reaper`'s defaults.

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

## Build (macOS)

Requires a C++17 compiler (Xcode command line tools), CMake ≥ 3.16, and
`libusb` + `rtmidi` (plus `liblo` for the Reaper bridge):

```sh
brew install cmake ninja libusb rtmidi liblo
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/command8-monitor               # loopback demo
./build/command8-reaper                # Reaper OSC bridge (identical OSC setup)
./build/command8-mackie                # MCU bridge (no loopback needed)
```

No `sudo` needed: on the machine this was verified on (macOS 15.6, Intel,
Homebrew, libusb 1.0.30) `command8-monitor` claims the USB interface and gets
live fader/encoder input and LED feedback as a normal user. If your setup
instead reports "device not found" or a claim failure, it's most likely
another process already holding the interface (see below) or a stricter USB
permission policy on your machine — try `sudo` as a fallback in that case, and
consider a `LaunchDaemon` if you need it every run.

If another Command|8 bridge is already running, stop it first: the interface is
exclusive.

### Mackie bridge on macOS

Nothing to install. `command8-mackie` publishes a virtual MIDI source and
destination, both named **`Command|8`**; point your DAW's Mackie Control
surface at that name for *both* its input and its output. Rename with
`--mcu-recv`/`--mcu-send` if you want something else.

Publishing both endpoints matters: with only a source, a DAW sees an input with
no matching output and control-surface support reports that it cannot find a
MIDI output.

## Build (Windows)

Requires Visual Studio 2022+ (MSVC), CMake, and vcpkg (all bundled with a
Visual Studio install). Dependencies (RtMidi, liblo) come from the vcpkg
manifest automatically:

```bat
cmake -S . -B build -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
ctest --test-dir build
build\command8-monitor.exe --list      # check the Command|8's ports are visible
build\command8-monitor.exe             # loopback demo
build\command8-reaper.exe              # Reaper OSC bridge (identical OSC setup)
build\command8-mackie.exe              # MCU bridge (see below)
```

(From a plain VS developer prompt, `%VCPKG_ROOT%` is
`%VSINSTALLDIR%VC\vcpkg`. The static triplet folds RtMidi, liblo and the MSVC
runtime into the exes; drop it for a plain dynamic dev build.)

Package a portable ZIP of the self-contained exes (`command8-<ver>-win64.zip`;
unzip anywhere, no vcredist or DLLs needed):

```bat
cd build && cpack
```

The Command|8 needs Digidesign/Avid's own driver on Windows to expose its MIDI
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


## License

GPL-3.0-or-later.
