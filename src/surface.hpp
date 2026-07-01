// SPDX-License-Identifier: GPL-3.0-or-later
// ALSA-sequencer I/O for the Command|8: finds the device by port name, wakes it,
// runs a timer-driven keepalive, decodes input into Events, and sends feedback.
#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

namespace command8 {

class Surface {
public:
    using EventCallback = std::function<void(const Event&)>;

    Surface() = default;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Open the ALSA client, find the device port (name substring), subscribe,
    // wake the surface and start the keepalive. Returns false on failure.
    bool open(const std::string& port_match = "Command|8 MIDI 1");
    void close();

    void set_callback(EventCallback cb) { cb_ = std::move(cb); }

    // Send a raw MIDI byte sequence to the surface (from protocol::* encoders).
    void send(const std::vector<uint8_t>& bytes);

    // Blocking input loop: decode incoming events and dispatch to the callback.
    // Heartbeats are filtered out (never delivered). Returns when stop() is called.
    void run();
    void stop() { running_ = false; }

    // Keepalive interval; the device drops output if not pinged periodically.
    std::chrono::milliseconds keepalive_interval{4000};

private:
    bool find_device_port(const std::string& match, int& client, int& port) const;
    void keepalive_loop();

    snd_seq_t* seq_ = nullptr;
    int my_port_ = -1;
    int dev_client_ = -1;
    int dev_port_ = -1;
    snd_midi_event_t* encoder_ = nullptr;   // raw-bytes -> seq-event encoder

    EventCallback cb_;
    std::thread keepalive_thread_;
    std::atomic<bool> running_{false};
    std::mutex out_mutex_;
};

}  // namespace command8
