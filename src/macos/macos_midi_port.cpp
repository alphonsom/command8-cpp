// SPDX-License-Identifier: GPL-3.0-or-later
#include "macos/macos_midi_port.hpp"

#if __has_include(<rtmidi/RtMidi.h>)
#include <rtmidi/RtMidi.h>
#else
#include <RtMidi.h>
#endif

#include <cstdio>

namespace command8 {

MacosMidiPort::~MacosMidiPort() { close(); }

bool MacosMidiPort::open(const std::string& in_match, const std::string& out_match) {
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

    try {
        // Create rather than find. The virtual destination is what the DAW
        // sends feedback to; the virtual source is what it receives on.
        in_->openVirtualPort(in_match);
        in_->ignoreTypes(false, true, true);   // MCU LCD feedback is SysEx
        out_->openVirtualPort(out_match);
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8-mcu: cannot create virtual MIDI port: %s\n",
                     e.getMessage().c_str());
        close();
        return false;
    }

    std::fprintf(stderr,
                 "command8-mcu: virtual MIDI ports created - select \"%s\" as BOTH "
                 "the input and output of your DAW's Mackie Control surface.\n",
                 in_match.c_str());
    if (in_match != out_match)
        std::fprintf(stderr, "command8-mcu: (recv \"%s\", send \"%s\")\n",
                     in_match.c_str(), out_match.c_str());
    return true;
}

void MacosMidiPort::close() {
    stop();
    if (in_) { in_->closePort(); in_.reset(); }
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        if (out_) { out_->closePort(); out_.reset(); }
    }
}

void MacosMidiPort::send(const std::vector<uint8_t>& bytes) {
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

void MacosMidiPort::midi_in_cb(double, std::vector<unsigned char>* msg, void* user) {
    auto* self = static_cast<MacosMidiPort*>(user);
    if (self->running_ && self->rx_ && msg && !msg->empty()) self->rx_(*msg);
}

void MacosMidiPort::start() {
    if (!in_ || running_) return;
    running_ = true;
    in_->setCallback(&MacosMidiPort::midi_in_cb, this);
}

void MacosMidiPort::stop() {
    running_ = false;
    // cancelCallback() unregisters before returning, so no rx_ call can begin
    // after this; the running_ gate covers one already past the registration.
    if (in_) in_->cancelCallback();
}

std::unique_ptr<MidiPort> make_midi_port() { return std::make_unique<MacosMidiPort>(); }

}  // namespace command8
