// SPDX-License-Identifier: GPL-3.0-or-later
// Platform-neutral I/O interface for the Command|8: find the device by port
// name, wake it, run a timer-driven keepalive, decode input into Events, and
// send feedback. Concrete implementations: AlsaSurface (Linux, ALSA sequencer)
// and RtMidiSurface (Windows, RtMidi/WinMM); make_surface() picks the platform
// default.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace command8 {

// Device port-name substring. The Linux name comes from the snd-usb-audio
// quirk. On Windows the surface is the device's first port, named exactly
// "Command|8" (the later ports show up as "MIDIIN2/3 (Command|8)"); the bar
// also keeps it from matching the "Command8 MCU" loopback endpoints.
// The value is unused wherever UsbSurface is the backend, which is everywhere
// libusb is available: it matches on VID/PID instead. No class driver on any
// platform successfully claims the MIDIStreaming interface -- on a stock Linux
// kernel snd-usb-audio binds neither interface, and the macOS and Windows class
// drivers reject it outright -- so there is no port to name and nothing to
// detach. On macOS there is no alternative backend at all.
#if defined(_WIN32)
inline constexpr const char* kDefaultPortMatch = "Command|8";
#elif defined(__APPLE__)
inline constexpr const char* kDefaultPortMatch = "";
#else
inline constexpr const char* kDefaultPortMatch = "Command|8 MIDI 1";
#endif

class Surface {
public:
    using EventCallback = std::function<void(const Event&)>;
    using TickCallback = std::function<void()>;

    virtual ~Surface() = default;

    // Open the MIDI backend, find the device port (name substring), wake the
    // surface and start the keepalive. Returns false on failure.
    virtual bool open(const std::string& port_match = kDefaultPortMatch) = 0;
    virtual void close() = 0;

    void set_callback(EventCallback cb) { cb_ = std::move(cb); }

    // Optional periodic callback, invoked from the input loop (~10 Hz) on the
    // same thread as the event callback (so it can safely touch shared state).
    void set_tick(TickCallback cb) { tick_cb_ = std::move(cb); }

    // Send a raw MIDI byte sequence to the surface (from protocol::* encoders).
    virtual void send(const std::vector<uint8_t>& bytes) = 0;

    // Blocking input loop: decode incoming events and dispatch to the callback,
    // run the tick, and watch for device removal. Heartbeats are filtered out.
    // Returns when stop() is called or the device disappears.
    virtual void run() = 0;
    virtual void stop() = 0;

    // Is the matched device port still present? (false after unplug.)
    virtual bool device_present() = 0;

    // Keepalive interval; the device drops output if not pinged periodically.
    std::chrono::milliseconds keepalive_interval{4000};

protected:
    EventCallback cb_;
    TickCallback tick_cb_;
};

// The platform's default Surface implementation (ALSA on Linux, RtMidi on
// Windows).
std::unique_ptr<Surface> make_surface();

// Diagnostic: print the MIDI ports the backend can see (stderr).
void print_midi_ports();

}  // namespace command8
