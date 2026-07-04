// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtmidi/rtmidi_surface.hpp"

#if __has_include(<rtmidi/RtMidi.h>)
#include <rtmidi/RtMidi.h>
#else
#include <RtMidi.h>
#endif

#include <cstdio>
#include <vector>

namespace command8 {

namespace {

// Port index for `match`: exact name wins, then a prefix match, then any
// substring match. WinMM names get an index suffix from RtMidi ("Command|8 0"),
// so the prefix tier is what usually picks the device port over
// "MIDIIN2 (Command|8) 1". -1 if none.
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

RtMidiSurface::~RtMidiSurface() { close(); }

bool RtMidiSurface::open(const std::string& port_match) {
    match_ = port_match;
    try {
        in_ = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "command8");
        out_ = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "command8");
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8: cannot init MIDI backend: %s\n",
                     e.getMessage().c_str());
        in_.reset();
        out_.reset();
        return false;
    }

    const int ip = find_port(*in_, port_match);
    const int op = find_port(*out_, port_match);
    if (ip < 0 || op < 0) {
        if (ip < 0 && op >= 0) {
            // The known-risk case from the Windows port: the class driver
            // enumerates the device's output but hides the input endpoint.
            std::fprintf(stderr,
                         "command8: found the device's MIDI OUT but no MIDI IN "
                         "matching \"%s\" - the class driver is hiding the "
                         "input endpoint.\n", port_match.c_str());
        } else {
            std::fprintf(stderr, "command8: no MIDI port matching \"%s\"\n",
                         port_match.c_str());
        }
        std::fprintf(stderr, "command8: available ports:\n");
        list_ports(*in_, "in");
        list_ports(*out_, "out");
        close();
        return false;
    }

    try {
        out_->openPort(static_cast<unsigned>(op), "command8 out");
        in_->openPort(static_cast<unsigned>(ip), "command8 in");
        in_->ignoreTypes(true, true, true);   // notes + CC only from the device
        in_->setCallback(&RtMidiSurface::midi_in_cb, this);
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8: cannot open MIDI port (%s) - is another "
                     "app (e.g. a DAW's MIDI input) holding the Command|8 MIDI "
                     "port?\n", e.getMessage().c_str());
        close();
        return false;
    }

    std::fprintf(stderr, "command8: surface open (in %d, out %d: %s)\n", ip, op,
                 in_->getPortName(static_cast<unsigned>(ip)).c_str());

    // wake the surface, then keep it online
    send(heartbeat());
    running_ = true;
    keepalive_thread_ = std::thread(&RtMidiSurface::keepalive_loop, this);
    return true;
}

void RtMidiSurface::close() {
    running_ = false;
    q_cv_.notify_all();
    if (keepalive_thread_.joinable()) keepalive_thread_.join();
    if (in_) { in_->cancelCallback(); in_->closePort(); in_.reset(); }
    if (out_) { out_->closePort(); out_.reset(); }
}

void RtMidiSurface::stop() {
    running_ = false;
    q_cv_.notify_all();
}

void RtMidiSurface::send(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    if (!out_) return;
    try {
        out_->sendMessage(&bytes);
    } catch (RtMidiError& e) {
        if (!send_warned_) {
            send_warned_ = true;
            std::fprintf(stderr, "command8: MIDI send failed: %s\n",
                         e.getMessage().c_str());
        }
    }
}

void RtMidiSurface::keepalive_loop() {
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

void RtMidiSurface::midi_in_cb(double, std::vector<unsigned char>* msg, void* user) {
    if (msg && msg->size() >= 3)
        static_cast<RtMidiSurface*>(user)->handle_message(*msg);
}

void RtMidiSurface::handle_message(const std::vector<unsigned char>& m) {
    Event decoded = std::monostate{};
    switch (m[0] & 0xF0) {
        case 0x90: decoded = decode_note_on(m[1], m[2]); break;
        case 0x80: decoded = decode_note_on(m[1], 0); break;   // note-off = release
        case 0xB0: decoded = decode_cc(m[1], m[2]); break;
        default:   return;
    }
    if (std::holds_alternative<HeartbeatEvent>(decoded)) return;   // filter
    if (std::holds_alternative<std::monostate>(decoded)) return;
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        queue_.push_back(decoded);
    }
    q_cv_.notify_one();
}

bool RtMidiSurface::device_present() {
    // WinMM re-enumerates on each query, so scanning the open handles' port
    // lists reflects unplug immediately.
    return in_ && out_ && find_port(*in_, match_) >= 0;
}

void RtMidiSurface::run() {
    if (!in_ || !out_) return;
    running_ = true;
    auto last_check = std::chrono::steady_clock::now();

    while (running_) {
        std::deque<Event> batch;
        {
            std::unique_lock<std::mutex> lock(q_mutex_);
            q_cv_.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return !queue_.empty() || !running_; });
            batch.swap(queue_);
        }
        for (const Event& ev : batch) {
            if (!running_) break;
            if (cb_) cb_(ev);
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

std::unique_ptr<Surface> make_surface() { return std::make_unique<RtMidiSurface>(); }

void print_midi_ports() {
    try {
        RtMidiIn in(RtMidi::UNSPECIFIED, "command8");
        RtMidiOut out(RtMidi::UNSPECIFIED, "command8");
        std::fprintf(stderr, "MIDI ports:\n");
        list_ports(in, "in");
        list_ports(out, "out");
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "command8: cannot init MIDI backend: %s\n",
                     e.getMessage().c_str());
    }
}

}  // namespace command8
