// SPDX-License-Identifier: GPL-3.0-or-later
// RtMidi MidiPort implementation (Windows / WinMM). Windows has no app-created
// virtual MIDI ports, so open a pre-created virtual cable: side A of a Windows
// MIDI Services loopback pair (the DAW takes side B), or a loopMIDI cable per
// direction. Rx callbacks are delivered on RtMidi's driver thread.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "midi_port.hpp"

class RtMidiIn;
class RtMidiOut;

namespace command8 {

class RtMidiMidiPort : public MidiPort {
public:
    RtMidiMidiPort() = default;
    ~RtMidiMidiPort() override;
    RtMidiMidiPort(const RtMidiMidiPort&) = delete;
    RtMidiMidiPort& operator=(const RtMidiMidiPort&) = delete;

    bool open(const std::string& in_match, const std::string& out_match) override;
    void close() override;
    bool ok() const override { return in_ != nullptr && out_ != nullptr; }
    void send(const std::vector<uint8_t>& bytes) override;
    void start() override;
    void stop() override;

private:
    static void midi_in_cb(double dt, std::vector<unsigned char>* msg, void* user);

    std::unique_ptr<RtMidiIn> in_;
    std::unique_ptr<RtMidiOut> out_;
    std::atomic<bool> running_{false};   // gate rx delivery between start/stop
    std::mutex out_mutex_;
    bool send_warned_ = false;           // log the first send failure only
};

}  // namespace command8
