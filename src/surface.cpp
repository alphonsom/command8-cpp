// SPDX-License-Identifier: GPL-3.0-or-later
#include "surface.hpp"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <vector>

namespace command8 {

Surface::~Surface() { close(); }

bool Surface::find_device_port(const std::string& match, int& client, int& port) const {
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq_, cinfo) >= 0) {
        const int c = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq_, pinfo) >= 0) {
            const std::string name = snd_seq_port_info_get_name(pinfo);
            const unsigned caps = snd_seq_port_info_get_capability(pinfo);
            // need a port we can both read from and write to
            const unsigned want = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE;
            if (name.find(match) != std::string::npos && (caps & want) == want) {
                client = c;
                port = snd_seq_port_info_get_port(pinfo);
                return true;
            }
        }
    }
    return false;
}

bool Surface::open(const std::string& port_match) {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        std::fprintf(stderr, "command8: cannot open ALSA sequencer\n");
        return false;
    }
    snd_seq_set_client_name(seq_, "command8");

    my_port_ = snd_seq_create_simple_port(
        seq_, "command8",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (my_port_ < 0) {
        std::fprintf(stderr, "command8: cannot create port\n");
        close();
        return false;
    }

    match_ = port_match;
    if (!find_device_port(port_match, dev_client_, dev_port_)) {
        std::fprintf(stderr, "command8: no port matching \"%s\"\n", port_match.c_str());
        close();
        return false;
    }

    // device -> us (input) and us -> device (output)
    const int rin = snd_seq_connect_from(seq_, my_port_, dev_client_, dev_port_);
    if (rin < 0) {
        std::fprintf(stderr,
                     "command8: cannot subscribe to the device input (%s) - is "
                     "another app (e.g. a DAW's MIDI input) holding the "
                     "Command|8 MIDI port?\n",
                     snd_strerror(rin));
        close();
        return false;
    }
    snd_seq_connect_to(seq_, my_port_, dev_client_, dev_port_);
    snd_seq_nonblock(seq_, 1);   // non-blocking input; the run() loop polls

    if (snd_midi_event_new(256, &encoder_) < 0) {
        std::fprintf(stderr, "command8: cannot create midi encoder\n");
        close();
        return false;
    }
    snd_midi_event_no_status(encoder_, 1);   // no running status

    std::fprintf(stderr, "command8: surface open (%d:%d)\n", dev_client_, dev_port_);

    // wake the surface, then keep it online
    send(heartbeat());
    running_ = true;
    keepalive_thread_ = std::thread(&Surface::keepalive_loop, this);
    return true;
}

void Surface::close() {
    running_ = false;
    if (keepalive_thread_.joinable()) keepalive_thread_.join();
    if (encoder_) { snd_midi_event_free(encoder_); encoder_ = nullptr; }
    if (seq_) { snd_seq_close(seq_); seq_ = nullptr; }
    my_port_ = dev_client_ = dev_port_ = -1;
}

void Surface::send(const std::vector<uint8_t>& bytes) {
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

void Surface::keepalive_loop() {
    // Timer-driven, never a reply: the device echoes host heartbeats, so replying
    // would create an echo loop. Sleep in small slices so stop() is responsive.
    auto next = std::chrono::steady_clock::now() + keepalive_interval;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::steady_clock::now() >= next) {
            send(heartbeat());
            next += keepalive_interval;
        }
    }
}

void Surface::dispatch_event(snd_seq_event_t* ev) {
    Event decoded = std::monostate{};
    switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            decoded = decode_note_on(ev->data.note.note, ev->data.note.velocity);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            decoded = decode_note_on(ev->data.note.note, 0);   // vel-0 = release
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            decoded = decode_cc(static_cast<uint8_t>(ev->data.control.param),
                                static_cast<uint8_t>(ev->data.control.value));
            break;
        default:
            return;
    }
    if (std::holds_alternative<HeartbeatEvent>(decoded)) return;   // filter
    if (std::holds_alternative<std::monostate>(decoded)) return;
    if (cb_) cb_(decoded);
}

bool Surface::device_present() {
    int c, p;
    return seq_ && find_device_port(match_, c, p);
}

void Surface::run() {
    if (!seq_) return;
    running_ = true;

    const int npfd = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> pfds(npfd > 0 ? npfd : 1);
    auto last_check = std::chrono::steady_clock::now();

    while (running_) {
        snd_seq_poll_descriptors(seq_, pfds.data(), pfds.size(), POLLIN);
        const int r = poll(pfds.data(), pfds.size(), 100);   // 100 ms slice
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r > 0) {
            snd_seq_event_t* ev = nullptr;
            while (running_ && snd_seq_event_input(seq_, &ev) >= 0) {
                if (ev) dispatch_event(ev);
            }
        }
        if (tick_cb_) tick_cb_();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check > std::chrono::milliseconds(500)) {
            last_check = now;
            if (!device_present()) {
                std::fprintf(stderr, "command8: device removed\n");
                running_ = false;
            }
        }
    }
}

}  // namespace command8
