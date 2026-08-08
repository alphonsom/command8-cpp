// SPDX-License-Identifier: GPL-3.0-or-later
// macOS MidiPort implementation (RtMidi / CoreMIDI, virtual ports).
//
// Unlike Windows and Linux, macOS lets an application create MIDI endpoints
// directly, so there is no loopback utility to install: the bridge publishes
// its own duplex pair and the DAW connects straight to it. open()'s match
// arguments are therefore used as the *names to create*, not names to search
// for. A DAW that opens both sides sees one device; the bridge never receives
// its own output, because a virtual source and a virtual destination are
// separate endpoints.
//
// Both endpoints must exist. Publishing only a source gives the DAW an input
// with no matching output, and control-surface support reports that it cannot
// find a MIDI output.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "midi_port.hpp"

class RtMidiIn;
class RtMidiOut;

namespace command8 {

class MacosMidiPort : public MidiPort {
public:
    MacosMidiPort() = default;
    ~MacosMidiPort() override;
    MacosMidiPort(const MacosMidiPort&) = delete;
    MacosMidiPort& operator=(const MacosMidiPort&) = delete;

    // in_match / out_match name the virtual ports to create. Passing the same
    // name for both (the usual case) publishes one duplex-looking device.
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
