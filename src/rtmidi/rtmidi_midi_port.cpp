// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtmidi/rtmidi_midi_port.hpp"

#if __has_include(<rtmidi/RtMidi.h>)
#include <rtmidi/RtMidi.h>
#else
#include <RtMidi.h>
#endif

#include <cstdio>

namespace command8 {

namespace {

// Exact name wins, then prefix, then substring (WinMM names carry an RtMidi
// index suffix, e.g. "Command8 MCU A 4"). -1 if none.
int find_port(RtMidi& io, const std::string& match) {
    const unsigned n = io.getPortCount();
    int prefix = -1, sub = -1;
    for (unsigned i = 0; i < n; ++i) {
        const std::string name = io.getPortName(i);
        if (name == match) return static_cast<int>(i);
        if (prefix < 0 && name.rfind(match, 0) == 0) prefix = static_cast<int>(i);
        if (sub < 0 && name.find(match) != std::string::npos)
            sub = static_cast<int>(i);
    }
    return prefix >= 0 ? prefix : sub;
}

void list_ports(RtMidi& io, const char* dir) {
    const unsigned n = io.getPortCount();
    for (unsigned i = 0; i < n; ++i)
        std::fprintf(stderr, "  [%s %u] %s\n", dir, i, io.getPortName(i).c_str());
}

}  // namespace

RtMidiMidiPort::~RtMidiMidiPort() { close(); }

bool RtMidiMidiPort::open(const std::string& in_match, const std::string& out_match) {
    try {
        in_ = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "command8-mcu");
        out_ = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "command8-mcu");
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8-mcu: cannot init MIDI backend: %s\n",
                     e.getMessage().c_str());
        in_.reset();
        out_.reset();
        return false;
    }

    const int ip = find_port(*in_, in_match);
    const int op = find_port(*out_, out_match);
    if (ip < 0 || op < 0) {
        std::fprintf(stderr, "command8-mcu: no MIDI port matching \"%s\" (recv) "
                     "and \"%s\" (send) - create the loopback pair first (midi "
                     "loopback create, or loopMIDI).\n",
                     in_match.c_str(), out_match.c_str());
        std::fprintf(stderr, "command8-mcu: available ports:\n");
        list_ports(*in_, "in");
        list_ports(*out_, "out");
        close();
        return false;
    }

    try {
        in_->openPort(static_cast<unsigned>(ip), "command8-mcu in");
        in_->ignoreTypes(false, true, true);   // MCU LCD feedback is SysEx
        out_->openPort(static_cast<unsigned>(op), "command8-mcu out");
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8-mcu: cannot open MIDI port: %s\n",
                     e.getMessage().c_str());
        close();
        return false;
    }
    std::fprintf(stderr, "command8-mcu: MCU recv \"%s\" send \"%s\"\n",
                 in_->getPortName(static_cast<unsigned>(ip)).c_str(),
                 out_->getPortName(static_cast<unsigned>(op)).c_str());
    return true;
}

void RtMidiMidiPort::close() {
    stop();
    if (in_) { in_->closePort(); in_.reset(); }
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        if (out_) { out_->closePort(); out_.reset(); }
    }
}

void RtMidiMidiPort::send(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    if (!out_) return;
    try {
        out_->sendMessage(&bytes);
    } catch (RtMidiError& e) {
        if (!send_warned_) {
            send_warned_ = true;
            std::fprintf(stderr, "command8-mcu: MIDI send failed: %s\n",
                         e.getMessage().c_str());
        }
    }
}

void RtMidiMidiPort::midi_in_cb(double, std::vector<unsigned char>* msg, void* user) {
    auto* self = static_cast<RtMidiMidiPort*>(user);
    if (self->running_ && self->rx_ && msg && !msg->empty()) self->rx_(*msg);
}

void RtMidiMidiPort::start() {
    if (!in_ || running_) return;
    running_ = true;
    in_->setCallback(&RtMidiMidiPort::midi_in_cb, this);
}

void RtMidiMidiPort::stop() {
    running_ = false;
    // cancelCallback() unregisters before returning, so no rx_ call can begin
    // after this; the running_ gate covers one already past the registration.
    if (in_) in_->cancelCallback();
}

std::unique_ptr<MidiPort> make_midi_port() { return std::make_unique<RtMidiMidiPort>(); }

}  // namespace command8
