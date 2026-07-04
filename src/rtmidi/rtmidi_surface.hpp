// SPDX-License-Identifier: GPL-3.0-or-later
// RtMidi Surface implementation (Windows / WinMM; RtMidi also builds on macOS
// and Linux). Input arrives on RtMidi's driver thread and is queued; run()
// drains the queue so the event callback and tick share one thread, matching
// the ALSA implementation's threading contract.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "surface.hpp"

class RtMidiIn;
class RtMidiOut;

namespace command8 {

class RtMidiSurface : public Surface {
public:
    RtMidiSurface() = default;
    ~RtMidiSurface() override;
    RtMidiSurface(const RtMidiSurface&) = delete;
    RtMidiSurface& operator=(const RtMidiSurface&) = delete;

    bool open(const std::string& port_match = kDefaultPortMatch) override;
    void close() override;
    void send(const std::vector<uint8_t>& bytes) override;
    void run() override;
    void stop() override;
    bool device_present() override;

private:
    static void midi_in_cb(double dt, std::vector<unsigned char>* msg, void* user);
    void handle_message(const std::vector<unsigned char>& m);
    void keepalive_loop();

    std::unique_ptr<RtMidiIn> in_;
    std::unique_ptr<RtMidiOut> out_;
    std::string match_;                     // device port-name substring

    std::thread keepalive_thread_;
    std::atomic<bool> running_{false};
    std::mutex out_mutex_;
    bool send_warned_ = false;              // log the first send failure only

    std::mutex q_mutex_;                    // guards queue_
    std::condition_variable q_cv_;
    std::deque<Event> queue_;
};

}  // namespace command8
