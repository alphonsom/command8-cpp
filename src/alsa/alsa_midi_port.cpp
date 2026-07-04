// SPDX-License-Identifier: GPL-3.0-or-later
#include "alsa/alsa_midi_port.hpp"

#include <poll.h>

#include <cstdio>
#include <memory>

namespace command8 {

AlsaMidiPort::~AlsaMidiPort() { close(); }

bool AlsaMidiPort::find_port(const std::string& match, unsigned need_caps,
                             int& client, int& port) const {
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq_, pi) >= 0) {
            const std::string name = snd_seq_port_info_get_name(pi);
            if (name.find(match) != std::string::npos &&
                (snd_seq_port_info_get_capability(pi) & need_caps) == need_caps) {
                client = c;
                port = snd_seq_port_info_get_port(pi);
                return true;
            }
        }
    }
    return false;
}

bool AlsaMidiPort::open(const std::string& in_match, const std::string& out_match) {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        seq_ = nullptr;
        return false;
    }
    snd_seq_set_client_name(seq_, "command8-mcu");
    my_port_ = snd_seq_create_simple_port(
        seq_, "MCU",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    int in_client = -1, in_port = -1, out_client = -1, out_port = -1;
    const bool found =
        my_port_ >= 0 &&
        find_port(in_match, SND_SEQ_PORT_CAP_READ, in_client, in_port) &&
        find_port(out_match, SND_SEQ_PORT_CAP_WRITE, out_client, out_port);
    if (!found) {
        std::fprintf(stderr, "command8-mcu: no MIDI port matching %s/%s (load "
                     "snd-virmidi?)\n", in_match.c_str(), out_match.c_str());
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }
    if (snd_midi_event_new(1024, &encoder_) < 0) {
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }
    snd_midi_event_no_status(encoder_, 1);

    snd_seq_connect_from(seq_, my_port_, in_client, in_port);
    snd_seq_connect_to(seq_, my_port_, out_client, out_port);
    snd_seq_nonblock(seq_, 1);
    std::fprintf(stderr, "command8-mcu: MCU port in %d:%d out %d:%d\n",
                 in_client, in_port, out_client, out_port);
    return true;
}

void AlsaMidiPort::close() {
    stop();
    if (encoder_) { snd_midi_event_free(encoder_); encoder_ = nullptr; }
    if (seq_) { snd_seq_close(seq_); seq_ = nullptr; }
    my_port_ = -1;
}

void AlsaMidiPort::send(const std::vector<uint8_t>& bytes) {
    if (!seq_ || !encoder_) return;
    std::lock_guard<std::mutex> lock(out_mutex_);
    const uint8_t* p = bytes.data();
    long remaining = static_cast<long>(bytes.size());
    while (remaining > 0) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        const long n = snd_midi_event_encode(encoder_, p, remaining, &ev);
        if (n <= 0) break;
        p += n;
        remaining -= n;
        if (ev.type == SND_SEQ_EVENT_NONE) continue;   // message not complete yet
        snd_seq_ev_set_source(&ev, my_port_);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq_, &ev);
    }
}

void AlsaMidiPort::start() {
    if (seq_ && !running_) {
        running_ = true;
        rx_thread_ = std::thread(&AlsaMidiPort::rx_loop, this);
    }
}

void AlsaMidiPort::stop() {
    running_ = false;
    if (rx_thread_.joinable()) rx_thread_.join();
}

// Convert a seq event back into raw bytes for the rx callback. Channel-voice
// events are rebuilt directly; SysEx fragments are reassembled until the F7.
void AlsaMidiPort::dispatch_event(const snd_seq_event_t* ev) {
    if (!rx_) return;
    std::vector<uint8_t> m;
    switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            m = {static_cast<uint8_t>(0x90 | (ev->data.note.channel & 0x0F)),
                 ev->data.note.note, ev->data.note.velocity};
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            m = {static_cast<uint8_t>(0x80 | (ev->data.note.channel & 0x0F)),
                 ev->data.note.note, ev->data.note.velocity};
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            m = {static_cast<uint8_t>(0xB0 | (ev->data.control.channel & 0x0F)),
                 static_cast<uint8_t>(ev->data.control.param & 0x7F),
                 static_cast<uint8_t>(ev->data.control.value & 0x7F)};
            break;
        case SND_SEQ_EVENT_CHANPRESS:
            m = {static_cast<uint8_t>(0xD0 | (ev->data.control.channel & 0x0F)),
                 static_cast<uint8_t>(ev->data.control.value & 0x7F)};
            break;
        case SND_SEQ_EVENT_PITCHBEND: {
            const int v = ev->data.control.value + 8192;   // 0..16383
            m = {static_cast<uint8_t>(0xE0 | (ev->data.control.channel & 0x0F)),
                 static_cast<uint8_t>(v & 0x7F), static_cast<uint8_t>((v >> 7) & 0x7F)};
            break;
        }
        case SND_SEQ_EVENT_SYSEX: {
            const auto* d = static_cast<const uint8_t*>(ev->data.ext.ptr);
            sysex_.insert(sysex_.end(), d, d + ev->data.ext.len);
            if (!sysex_.empty() && sysex_.back() == 0xF7) {
                rx_(sysex_);
                sysex_.clear();
            }
            return;
        }
        default:
            return;
    }
    rx_(m);
}

void AlsaMidiPort::rx_loop() {
    const int npfd = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> pfds(npfd > 0 ? npfd : 1);
    while (running_) {
        snd_seq_poll_descriptors(seq_, pfds.data(), pfds.size(), POLLIN);
        if (poll(pfds.data(), pfds.size(), 100) <= 0) continue;
        snd_seq_event_t* ev = nullptr;
        while (running_ && snd_seq_event_input(seq_, &ev) >= 0) {
            if (ev) dispatch_event(ev);
        }
    }
}

std::unique_ptr<MidiPort> make_midi_port() { return std::make_unique<AlsaMidiPort>(); }

}  // namespace command8
