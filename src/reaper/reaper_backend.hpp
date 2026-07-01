// SPDX-License-Identifier: GPL-3.0-or-later
// Reaper host integration: a Backend speaking Reaper's OSC (via liblo), matching
// reaper/Command8.ReaperOSC. Built on libcommand8; the core has no OSC/DAW dep.
//
// Ports the command8-linux Python `reaper` profile: 5 encoder-assign modes
// (Pan/Send/Insert/EQ/Dynamics), Flip, nav modes (Bank/Nudge/Zoom), the LCD
// name/value grid, Display-Mode hold, ring-on-knob-turn, transport, RecSel arm,
// and the action buttons. (Not ported: the temporary fader-value LCD flash.)
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
    enum class Enc { Pan, Send, Insert, EQ, Dyn };
    enum class Nav { None, Bank, Nudge, Zoom };

    // --- OSC ---
    void send_f(const std::string& path, float v);
    void send_i(const std::string& path, int v);
    static int osc_cb(const char* path, const char* types, lo_arg** argv,
                      int argc, lo_message msg, void* user);
    void handle(const char* path, const char* types, lo_arg** argv, int argc);

    // --- helpers (mirror reaper.py) ---
    bool is_fx() const { return enc_mode_ != Enc::Pan && enc_mode_ != Enc::Send; }
    bool flipped() const { return flip_ && !is_fx(); }
    const char* enc_suffix() const;         // "pan" | "send/1/volume"
    double active_value(int strip) const;
    void set_active(int strip, double v);
    std::string fmt_pan(double v) const;
    std::string fmt_pct(double v) const;
    std::string fmt_active(int strip) const;
    std::string param_name(int enc) const;

    void value_cell(int strip);             // paint value (FX: bottom / else top)
    void label_cell(int strip);             // paint label (FX: top name / else track)
    void repaint();
    void active_ring(int enc, double v);     // dot for Pan, fill otherwise

    void led(const std::string& name, bool on);
    void enc_mode_leds();
    void nav_mode_leds();
    void update_recsel_led();
    void set_enc_mode(Enc m);
    void handle_nav(const std::string& arrow, bool pressed);

    // --- config ---
    std::string host_, send_port_, recv_port_;
    lo_address tx_ = nullptr;
    lo_server_thread rx_ = nullptr;
    static constexpr double PAN_STEP = 0.02;
    static constexpr double PAN_STEP_FINE = 0.005;
    static constexpr int DYN_FX_SLOT = 2;

    // --- state (guarded by m_) ---
    std::mutex m_;
    Enc enc_mode_ = Enc::Pan;
    Nav nav_mode_ = Nav::None;
    bool flip_ = false;
    bool disp_hold_ = false;
    std::array<double, 8> pan_, vol_, send_, fxp_, eq_;
    std::array<std::string, 8> names_, fxname_;
    std::set<int> selected_, recarm_;
    std::set<std::string> held_mods_;
};

}  // namespace command8
