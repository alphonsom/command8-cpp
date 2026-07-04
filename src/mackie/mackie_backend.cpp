// SPDX-License-Identifier: GPL-3.0-or-later
#include "mackie/mackie_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "feedback.hpp"

namespace command8 {

namespace {
// MCU note map
constexpr int N_REC = 0, N_SOLO = 8, N_MUTE = 16, N_SELECT = 24;
constexpr int N_PLAY = 0x5E, N_STOP = 0x5D, N_REC_BTN = 0x5F;
constexpr int N_REW = 0x5B, N_FFWD = 0x5C, N_CYCLE = 0x56;
constexpr int N_BANK_L = 0x2E, N_BANK_R = 0x2F;
constexpr int VPOT_CC = 0x10, VPOT_LED_CC = 0x30;
// V-pot assignment section
constexpr int N_SEND = 0x29, N_PAN = 0x2A, N_PLUGIN = 0x2B, N_EQ = 0x2C, N_INST = 0x2D;
// nav / view
constexpr int N_FLIP = 0x32;
constexpr int N_CHAN_L = 0x30, N_CHAN_R = 0x31;   // move by 1 track
constexpr int N_CUR_UP = 0x60, N_CUR_DN = 0x61, N_CUR_L = 0x62, N_CUR_R = 0x63;
constexpr int N_ZOOM = 0x64;

// Navigation cluster (all at subid 13): Bank/Nudge/Zoom are a local mode radio
// group; ScrlBack/ScrollFwd and ViewUP/Down translate per the active mode. These
// are handled specially in on_button (not via kBtnToMcu below).
constexpr int NAV_SUBID = 13;
constexpr int BTN_BANK = 2, BTN_NUDGE = 3, BTN_ZOOM = 4;
constexpr int BTN_SCRL_BACK = 5, BTN_SCRL_FWD = 6, BTN_VIEW_UP = 7, BTN_VIEW_DN = 8;
enum NavMode { NAV_BANK = 0, NAV_NUDGE = 1, NAV_ZOOM = 2 };

// Command|8 discrete (note,subid) -> MCU note. RecSel and the nav cluster are
// special-cased in on_button.
const std::map<std::pair<int, int>, int> kBtnToMcu = {
    {{10, 14}, N_PLAY}, {{9, 14}, N_STOP},   {{11, 14}, N_REC_BTN},
    {{3, 14}, N_CYCLE}, {{7, 14}, N_REW},    {{8, 14}, N_FFWD},
    // V-pot assignment (Pan/Send/Insert/EQ/Dynamics)
    {{0, 10}, N_PAN}, {{1, 10}, N_SEND}, {{2, 10}, N_PLUGIN},
    {{0, 11}, N_EQ},  {{1, 11}, N_INST},
    // Flip (independent toggle, not part of the nav mode group)
    {{0, 13}, N_FLIP},
};
// MCU note -> Command|8 LED (note, subid) for feedback. The nav mode LEDs
// (Bank/Nudge/Zoom) are driven locally, not from DAW feedback.
const std::map<int, std::pair<int, int>> kMcuToLed = {
    {N_PLAY, {10, 14}}, {N_STOP, {9, 14}}, {N_REC_BTN, {11, 14}}, {N_CYCLE, {3, 14}},
    {N_PAN, {0, 10}}, {N_SEND, {1, 10}}, {N_PLUGIN, {2, 10}}, {N_EQ, {0, 11}},
    {N_INST, {1, 11}}, {N_FLIP, {0, 13}},
};
}  // namespace

MackieBackend::MackieBackend(std::string recv_match, std::string send_match)
    : port_(make_midi_port()) {
    if (!port_->open(recv_match, send_match)) port_.reset();
    if (port_) port_->set_rx([this](const std::vector<uint8_t>& m) { handle_mcu(m); });
}

MackieBackend::~MackieBackend() { stop(); }

void MackieBackend::on_start() {
    lcd_.fill(' ');
    nav_mode_ = NAV_BANK;
    paint_nav_leds();     // light the default nav mode (Bank)
    if (port_) port_->start();
}

void MackieBackend::stop() {
    if (port_) port_->stop();
}

// --- send helpers ----------------------------------------------------------
void MackieBackend::send_note(int note, bool on) {
    if (!port_) return;
    port_->send({0x90, static_cast<uint8_t>(note & 0x7F),
                 static_cast<uint8_t>(on ? 127 : 0)});
}
void MackieBackend::send_cc(int cc, int value) {
    if (!port_) return;
    port_->send({0xB0, static_cast<uint8_t>(cc & 0x7F),
                 static_cast<uint8_t>(value & 0x7F)});
}
void MackieBackend::send_pitch(int channel, int value) {
    if (!port_) return;
    const int v = std::max(-8192, std::min(8191, value)) + 8192;   // 0..16383
    port_->send({static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
                 static_cast<uint8_t>(v & 0x7F),
                 static_cast<uint8_t>((v >> 7) & 0x7F)});
}

// --- Command|8 surface -> MCU ---------------------------------------------
void MackieBackend::on_fader(int strip, double v) {
    int pitch = static_cast<int>(std::lround(v * 16383.0)) - 8192;
    send_pitch(strip & 7, pitch);
}
void MackieBackend::on_encoder(int strip, int delta) {
    send_cc(VPOT_CC + (strip & 7), delta > 0 ? 1 : (0x40 | 1));
}
void MackieBackend::on_select(int strip, bool pressed) { send_note(N_SELECT + (strip & 7), pressed); }
void MackieBackend::on_mute(int strip, bool pressed)   { send_note(N_MUTE + (strip & 7), pressed); }
void MackieBackend::on_solo(int strip, bool pressed)   { send_note(N_SOLO + (strip & 7), pressed); }

void MackieBackend::on_button(uint8_t note, uint8_t subid, bool pressed) {
    if (note == 3 && subid == 12) {   // RecSel -> arm the selected track(s)
        std::lock_guard<std::mutex> lk(state_m_);
        for (int ch : selected_) send_note(N_REC + ch, pressed);
        return;
    }
    if (subid == NAV_SUBID) {
        switch (note) {
            case BTN_BANK:  if (pressed) set_nav_mode(NAV_BANK);  return;
            case BTN_NUDGE: if (pressed) set_nav_mode(NAV_NUDGE); return;
            case BTN_ZOOM:  if (pressed) set_nav_mode(NAV_ZOOM);  return;
            case BTN_SCRL_BACK:   // '<' : per-mode step left
                send_note(nav_mode_ == NAV_BANK ? N_BANK_L
                          : nav_mode_ == NAV_NUDGE ? N_CHAN_L : N_CUR_L, pressed);
                return;
            case BTN_SCRL_FWD:    // '>' : per-mode step right
                send_note(nav_mode_ == NAV_BANK ? N_BANK_R
                          : nav_mode_ == NAV_NUDGE ? N_CHAN_R : N_CUR_R, pressed);
                return;
            case BTN_VIEW_UP: send_note(N_CUR_UP, pressed); return;  // zoom in Zoom mode
            case BTN_VIEW_DN: send_note(N_CUR_DN, pressed); return;
            default: break;    // Flip (note 0), MstrFadrs (note 1): fall through
        }
    }
    auto it = kBtnToMcu.find({note, subid});
    if (it != kBtnToMcu.end()) send_note(it->second, pressed);
}

// Bank/Nudge/Zoom radio group. Only the active mode's LED lights; entering or
// leaving Zoom toggles the DAW's Zoom modifier so the arrows zoom.
void MackieBackend::set_nav_mode(int mode) {
    if (mode == nav_mode_) return;
    const bool was_zoom = (nav_mode_ == NAV_ZOOM), now_zoom = (mode == NAV_ZOOM);
    nav_mode_ = mode;
    if (now_zoom != was_zoom) { send_note(N_ZOOM, true); send_note(N_ZOOM, false); }
    paint_nav_leds();
}
void MackieBackend::paint_nav_leds() {
    if (!fb_) return;
    fb_->strip_led(BTN_BANK,  NAV_SUBID, nav_mode_ == NAV_BANK);
    fb_->strip_led(BTN_NUDGE, NAV_SUBID, nav_mode_ == NAV_NUDGE);
    fb_->strip_led(BTN_ZOOM,  NAV_SUBID, nav_mode_ == NAV_ZOOM);
}

// --- MCU (DAW feedback) -> Command|8 --------------------------------------
void MackieBackend::handle_mcu(const std::vector<uint8_t>& m) {
    if (!fb_ || m.empty()) return;
    if (m[0] == 0xF0) {
        lcd_sysex(m.data(), static_cast<int>(m.size()));
        return;
    }
    const int type = m[0] & 0xF0, ch = m[0] & 0x0F;
    switch (type) {
        case 0xE0: {                                    // pitchbend = fader
            if (m.size() < 3) break;
            const int v = (m[2] << 7) | m[1];           // 0..16383
            if (ch < 8) fb_->fader(ch, v / 16383.0);
            break;
        }
        case 0xD0: {                                    // channel pressure = meters
            if (m.size() < 2) break;
            const int v = m[1];
            const int strip = (v >> 4) & 7, level = v & 0x0F;
            fb_->meter(strip, std::min(1.0, level / 12.0));
            break;
        }
        case 0xB0: {
            if (m.size() < 3) break;
            const int cc = m[1];
            if (cc >= VPOT_LED_CC && cc <= VPOT_LED_CC + 7) {
                const int enc = cc - VPOT_LED_CC, val = m[2];
                const int pos = val & 0x0F, mode = (val >> 4) & 0x03;
                if (pos == 0) fb_->ring_fill(enc, 0.0);
                else if (mode == 2) fb_->ring_fill(enc, pos / 11.0);      // wrap/fill
                else fb_->ring_dot(enc, (pos - 1) / 10.0);               // single dot
            }
            break;
        }
        case 0x90:
        case 0x80: {
            if (m.size() < 3) break;
            const bool on = (type == 0x90 && m[2] > 0);
            const int n = m[1];
            if (n >= N_REC && n <= N_REC + 7) fb_->recarm_led(n - N_REC, on);
            else if (n >= N_SOLO && n <= N_SOLO + 7) fb_->solo_led(n - N_SOLO, on);
            else if (n >= N_MUTE && n <= N_MUTE + 7) fb_->mute_led(n - N_MUTE, on);
            else if (n >= N_SELECT && n <= N_SELECT + 7) {
                const int c = n - N_SELECT;
                { std::lock_guard<std::mutex> lk(state_m_);
                  if (on) selected_.insert(c); else selected_.erase(c); }
                fb_->select_led(c, on);
            } else {
                auto it = kMcuToLed.find(n);
                if (it != kMcuToLed.end())
                    fb_->strip_led(static_cast<uint8_t>(it->second.first),
                                   it->second.second, on);
            }
            break;
        }
        default:
            break;
    }
}

void MackieBackend::lcd_sysex(const uint8_t* d, int len) {
    // F0 00 00 66 <model> 12 <offset> <ascii...> F7
    if (len < 8 || d[1] != 0x00 || d[2] != 0x00 || d[3] != 0x66 || d[5] != 0x12)
        return;
    int offset = d[6] & 0x7F;
    int n = len - 8;   // strip F0..header(6) and trailing F7
    std::set<std::pair<int, int>> cells;
    for (int i = 0; i < n; ++i) {
        int p = offset + i;
        if (p < 0 || p >= 112) continue;
        uint8_t ch = d[7 + i];
        lcd_[p] = (ch >= 0x20 && ch <= 0x7E) ? ch : ' ';
        cells.insert({p / 56, (p % 56) / 7});
    }
    for (auto& [line, col] : cells) {
        if (col > 7) continue;
        int base = line * 56 + col * 7;
        std::string s(reinterpret_cast<char*>(&lcd_[base]), 7);
        if (line == 0) fb_->lcd_status(col, s);
        else fb_->lcd_channel(col, s);
    }
}

}  // namespace command8
