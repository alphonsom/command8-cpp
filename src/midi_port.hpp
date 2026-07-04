// SPDX-License-Identifier: GPL-3.0-or-later
// Bidirectional raw-bytes MIDI port, found by port-name substring. Used by the
// Mackie backend for its MCU-facing port so the MCU translation logic is
// transport-agnostic. Implementations: ALSA sequencer (Linux, snd-virmidi) and
// RtMidi (Windows, loopMIDI); make_midi_port() picks the platform default.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace command8 {

class MidiPort {
public:
    // One complete MIDI message (channel voice or full F0..F7 SysEx). May be
    // invoked from an internal receive thread.
    using RxCallback = std::function<void(const std::vector<uint8_t>&)>;

    virtual ~MidiPort() = default;

    // in_match: port we receive DAW-to-bridge data on; out_match: port we send
    // bridge-to-DAW data to. Usually both name the same duplex port: a virmidi
    // port on ALSA, or side A of a Windows MIDI Services loopback pair (a
    // crossed pair, so the bridge never hears its own output). Separate names
    // support two-cable setups like loopMIDI, which echoes a single cable back
    // to every reader.
    virtual bool open(const std::string& in_match, const std::string& out_match) = 0;
    virtual void close() = 0;
    virtual bool ok() const = 0;

    virtual void send(const std::vector<uint8_t>& bytes) = 0;

    void set_rx(RxCallback cb) { rx_ = std::move(cb); }
    virtual void start() = 0;   // begin delivering rx callbacks
    virtual void stop() = 0;    // stop rx delivery (joins any rx thread)

protected:
    RxCallback rx_;
};

// The platform's default MidiPort implementation.
std::unique_ptr<MidiPort> make_midi_port();

}  // namespace command8
