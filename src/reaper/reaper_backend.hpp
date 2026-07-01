// SPDX-License-Identifier: GPL-3.0-or-later
// Reaper host integration: a Backend that speaks Reaper's OSC (via liblo),
// matching the reaper/Command8.ReaperOSC pattern config. This is a front-end
// built on libcommand8; the core library has no OSC/DAW dependency.
//
// v1 scope: strips (fader->volume, encoder->pan, mute/solo/select toggle),
// RecSel record-arms the selected track(s), transport (Play/Stop/LoopPlay), and
// feedback (volume->fader, pan->ring dot, mute/solo/select/recarm LEDs,
// monitor->surface LED, vu->meter, track name->LCD, transport LEDs).
// TODO (v2): encoder modes Send/Insert/EQ/Dynamics, Flip, nav modes, LCD
// name/value grid, action buttons (RTZ/Undo/WMix/...).
#pragma once

#include <lo/lo.h>

#include <array>
#include <mutex>
#include <set>
#include <string>

#include "backend.hpp"

namespace command8 {

class ReaperBackend : public Backend {
public:
    ReaperBackend(std::string host = "127.0.0.1",
                  std::string send_port = "8000",
                  std::string recv_port = "9000");
    ~ReaperBackend() override;

    void on_start() override;
    void on_fader(int strip, double value01) override;
    void on_encoder(int strip, int delta) override;
    void on_select(int strip, bool pressed) override;
    void on_mute(int strip, bool pressed) override;
    void on_solo(int strip, bool pressed) override;
    void on_button(uint8_t note, uint8_t subid, bool pressed) override;

private:
    void send_f(const std::string& path, float v);
    void handle(const char* path, const char* types, lo_arg** argv, int argc);
    static int osc_cb(const char* path, const char* types, lo_arg** argv,
                      int argc, lo_message msg, void* user);
    void led_for(const std::string& name, bool on);
    void update_recsel_led();

    std::string host_, send_port_, recv_port_;
    lo_address tx_ = nullptr;
    lo_server_thread rx_ = nullptr;

    static constexpr double PAN_STEP = 0.02;
    std::array<double, 8> pan_;
    std::mutex m_;               // guards selected_/recarm_
    std::set<int> selected_, recarm_;
};

}  // namespace command8
