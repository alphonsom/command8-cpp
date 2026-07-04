// SPDX-License-Identifier: GPL-3.0-or-later
// ALSA-sequencer MidiPort implementation (Linux). Typically opened on a
// snd-virmidi "Virtual Raw MIDI" kernel port, which DAWs like Bitwig can see.
#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "midi_port.hpp"

namespace command8 {

class AlsaMidiPort : public MidiPort {
public:
    AlsaMidiPort() = default;
    ~AlsaMidiPort() override;
    AlsaMidiPort(const AlsaMidiPort&) = delete;
    AlsaMidiPort& operator=(const AlsaMidiPort&) = delete;

    bool open(const std::string& in_match, const std::string& out_match) override;
    void close() override;
    bool ok() const override { return seq_ != nullptr; }
    void send(const std::vector<uint8_t>& bytes) override;
    void start() override;
    void stop() override;

private:
    bool find_port(const std::string& match, unsigned need_caps,
                   int& client, int& port) const;
    void rx_loop();
    void dispatch_event(const snd_seq_event_t* ev);

    snd_seq_t* seq_ = nullptr;
    int my_port_ = -1;
    snd_midi_event_t* encoder_ = nullptr;   // raw-bytes -> seq-event encoder

    std::thread rx_thread_;
    std::atomic<bool> running_{false};
    std::mutex out_mutex_;
    std::vector<uint8_t> sysex_;            // reassembles fragmented SysEx
};

}  // namespace command8
