// SPDX-License-Identifier: GPL-3.0-or-later
// ALSA-sequencer Surface implementation (Linux).
#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "surface.hpp"

namespace command8 {

class AlsaSurface : public Surface {
public:
    AlsaSurface() = default;
    ~AlsaSurface() override;
    AlsaSurface(const AlsaSurface&) = delete;
    AlsaSurface& operator=(const AlsaSurface&) = delete;

    bool open(const std::string& port_match = kDefaultPortMatch) override;
    void close() override;
    void send(const std::vector<uint8_t>& bytes) override;
    void run() override;
    void stop() override { running_ = false; }
    bool device_present() override;

private:
    bool find_device_port(const std::string& match, int& client, int& port) const;
    void keepalive_loop();
    void dispatch_event(snd_seq_event_t* ev);

    snd_seq_t* seq_ = nullptr;
    int my_port_ = -1;
    int dev_client_ = -1;
    int dev_port_ = -1;
    snd_midi_event_t* encoder_ = nullptr;   // raw-bytes -> seq-event encoder
    std::string match_;                     // device port-name substring

    std::thread keepalive_thread_;
    std::atomic<bool> running_{false};
    std::mutex out_mutex_;
};

// List the ALSA sequencer ports. Always available so the libusb backend's
// diagnostics can fall back to it under COMMAND8_BACKEND=alsa.
void alsa_print_midi_ports();

}  // namespace command8
