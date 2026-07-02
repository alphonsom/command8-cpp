// SPDX-License-Identifier: GPL-3.0-or-later
// Mackie Control (MCU) emulation backend. Presents the Command|8 to the DAW as a
// Mackie Control on a MIDI port (typically a snd-virmidi "Virtual Raw MIDI"
// kernel port, which DAWs like Bitwig can see). Built on libcommand8; uses ALSA
// seq directly for the MCU port (no extra dependency).
//
// See command8-linux command8/mackie.py for the reference translation.
#pragma once

#include <alsa/asoundlib.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "backend.hpp"

namespace command8 {

class MackieBackend : public Backend {
public:
    explicit MackieBackend(std::string port_match = "VirMIDI");
    ~MackieBackend() override;

    bool ok() const { return seq_ != nullptr; }   // MCU port opened?

    void on_start() override;                      // starts the MCU receive loop
    void stop();                                   // stop rx before Feedback dies

    void on_fader(int strip, double value01) override;
    void on_encoder(int strip, int delta) override;
    void on_select(int strip, bool pressed) override;
    void on_mute(int strip, bool pressed) override;
    void on_solo(int strip, bool pressed) override;
    void on_button(uint8_t note, uint8_t subid, bool pressed) override;

private:
    bool open_port();
    void send_note(int note, bool on);
    void send_cc(int cc, int value);
    void send_pitch(int channel, int value);
    void send_event(snd_seq_event_t* ev);
    void rx_loop();
    void handle_mcu(const snd_seq_event_t* ev);
    void lcd_sysex(const uint8_t* data, int len);
    void set_nav_mode(int mode);   // Bank/Nudge/Zoom radio group
    void paint_nav_leds();

    std::string match_;
    int nav_mode_ = 0;               // 0=Bank,1=Nudge,2=Zoom (input thread only)
    snd_seq_t* seq_ = nullptr;
    int my_port_ = -1, dev_client_ = -1, dev_port_ = -1;

    std::thread rx_thread_;
    std::atomic<bool> running_{false};
    std::mutex out_m_;                 // guards seq output
    std::mutex state_m_;              // guards selected_
    std::set<int> selected_;
    std::array<uint8_t, 112> lcd_{};  // Mackie 2x56 LCD buffer
};

}  // namespace command8
