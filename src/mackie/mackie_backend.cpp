// SPDX-License-Identifier: GPL-3.0-or-later
#include "mackie/mackie_backend.hpp"

#include <poll.h>

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

int find_port(snd_seq_t* seq, const std::string& match, int& client, int& port) {
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            const std::string name = snd_seq_port_info_get_name(pi);
            const unsigned want = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE;
            if (name.find(match) != std::string::npos &&
                (snd_seq_port_info_get_capability(pi) & want) == want) {
                client = c;
                port = snd_seq_port_info_get_port(pi);
                return 0;
            }
        }
    }
    return -1;
}
}  // namespace

MackieBackend::MackieBackend(std::string port_match) : match_(std::move(port_match)) {
    open_port();
}

MackieBackend::~MackieBackend() {
    stop();
    if (seq_) { snd_seq_close(seq_); seq_ = nullptr; }
}

bool MackieBackend::open_port() {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        seq_ = nullptr;
        return false;
    }
    snd_seq_set_client_name(seq_, "command8-mcu");
    my_port_ = snd_seq_create_simple_port(
        seq_, "MCU",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (my_port_ < 0 || find_port(seq_, match_, dev_client_, dev_port_) < 0) {
        std::fprintf(stderr, "command8-mcu: no MIDI port matching %s (load "
                     "snd-virmidi?)\n", match_.c_str());
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }
    snd_seq_connect_from(seq_, my_port_, dev_client_, dev_port_);
    snd_seq_connect_to(seq_, my_port_, dev_client_, dev_port_);
    snd_seq_nonblock(seq_, 1);
    std::fprintf(stderr, "command8-mcu: MCU port %s (%d:%d)\n", match_.c_str(),
                 dev_client_, dev_port_);
    return true;
}

void MackieBackend::on_start() {
    lcd_.fill(' ');
    nav_mode_ = NAV_BANK;
    paint_nav_leds();     // light the default nav mode (Bank)
    if (seq_ && !running_) {
        running_ = true;
        rx_thread_ = std::thread(&MackieBackend::rx_loop, this);
    }
}

void MackieBackend::stop() {
    running_ = false;
    if (rx_thread_.joinable()) rx_thread_.join();
}

// --- send helpers ----------------------------------------------------------
void MackieBackend::send_event(snd_seq_event_t* ev) {
    if (!seq_) return;
    std::lock_guard<std::mutex> lk(out_m_);
    snd_seq_ev_set_source(ev, my_port_);
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output_direct(seq_, ev);
}
void MackieBackend::send_note(int note, bool on) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev, 0, note, on ? 127 : 0);
    send_event(&ev);
}
void MackieBackend::send_cc(int cc, int value) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, 0, cc, value);
    send_event(&ev);
}
void MackieBackend::send_pitch(int channel, int value) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_pitchbend(&ev, channel, value);   // -8192..8191
    send_event(&ev);
}

// --- Command|8 surface -> MCU ---------------------------------------------
void MackieBackend::on_fader(int strip, double v) {
    int pitch = static_cast<int>(std::lround(v * 16383.0)) - 8192;
    pitch = std::max(-8192, std::min(8191, pitch));
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
void MackieBackend::rx_loop() {
    const int npfd = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> pfds(npfd > 0 ? npfd : 1);
    while (running_) {
        snd_seq_poll_descriptors(seq_, pfds.data(), pfds.size(), POLLIN);
        if (poll(pfds.data(), pfds.size(), 100) <= 0) continue;
        snd_seq_event_t* ev = nullptr;
        while (running_ && snd_seq_event_input(seq_, &ev) >= 0) {
            if (ev) handle_mcu(ev);
        }
    }
}

void MackieBackend::handle_mcu(const snd_seq_event_t* ev) {
    if (!fb_) return;
    switch (ev->type) {
        case SND_SEQ_EVENT_PITCHBEND: {
            int ch = ev->data.control.channel;
            if (ch < 8)
                fb_->fader(ch, (ev->data.control.value + 8192) / 16383.0);
            break;
        }
        case SND_SEQ_EVENT_CHANPRESS: {                 // meters
            int v = ev->data.control.value;
            int ch = (v >> 4) & 7, level = v & 0x0F;
            fb_->meter(ch, std::min(1.0, level / 12.0));
            break;
        }
        case SND_SEQ_EVENT_CONTROLLER: {
            int cc = ev->data.control.param;
            if (cc >= VPOT_LED_CC && cc <= VPOT_LED_CC + 7) {
                int enc = cc - VPOT_LED_CC, val = ev->data.control.value;
                int pos = val & 0x0F, mode = (val >> 4) & 0x03;
                if (pos == 0) fb_->ring_fill(enc, 0.0);
                else if (mode == 2) fb_->ring_fill(enc, pos / 11.0);      // wrap/fill
                else fb_->ring_dot(enc, (pos - 1) / 10.0);               // single dot
            }
            break;
        }
        case SND_SEQ_EVENT_NOTEON:
        case SND_SEQ_EVENT_NOTEOFF: {
            bool on = (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0);
            int n = ev->data.note.note;
            if (n >= N_REC && n <= N_REC + 7) fb_->recarm_led(n - N_REC, on);
            else if (n >= N_SOLO && n <= N_SOLO + 7) fb_->solo_led(n - N_SOLO, on);
            else if (n >= N_MUTE && n <= N_MUTE + 7) fb_->mute_led(n - N_MUTE, on);
            else if (n >= N_SELECT && n <= N_SELECT + 7) {
                int ch = n - N_SELECT;
                { std::lock_guard<std::mutex> lk(state_m_);
                  if (on) selected_.insert(ch); else selected_.erase(ch); }
                fb_->select_led(ch, on);
            } else {
                auto it = kMcuToLed.find(n);
                if (it != kMcuToLed.end())
                    fb_->strip_led(static_cast<uint8_t>(it->second.first),
                                   it->second.second, on);
            }
            break;
        }
        case SND_SEQ_EVENT_SYSEX:
            lcd_sysex(static_cast<const uint8_t*>(ev->data.ext.ptr), ev->data.ext.len);
            break;
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
