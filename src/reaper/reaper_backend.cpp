// SPDX-License-Identifier: GPL-3.0-or-later
#include "reaper/reaper_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#include "feedback.hpp"

namespace command8 {

namespace {

// (note, subid) -> button label, from command8-linux command8_buttons.json.
const std::map<std::pair<int, int>, std::string> kButtons = {
    {{0, 10}, "Pan"},   {{0, 11}, "EQ"},        {{0, 12}, "Enter"},
    {{0, 13}, "Flip"},  {{0, 14}, "WPlugin"},   {{1, 10}, "Send"},
    {{1, 11}, "Dynamics"}, {{1, 12}, "Undo"},   {{1, 13}, "MstrFadrs"},
    {{1, 14}, "WMix"},  {{10, 10}, "mod_ctrl"}, {{10, 14}, "Play"},
    {{11, 10}, "mod_cmd"}, {{12, 10}, "Default"}, {{12, 14}, "MemLoc"},
    {{13, 10}, "Mon/0"}, {{2, 10}, "Insert"},   {{2, 13}, "Bank"},
    {{2, 14}, "WEdit"}, {{3, 10}, "ConA"},      {{3, 11}, "Pn/Snd/Pre"},
    {{3, 12}, "RecSel"}, {{3, 13}, "Nudge"},    {{3, 14}, "LoopPlay"},
    {{4, 10}, "ConB"},  {{4, 11}, "Page<"},     {{4, 12}, "Pan/Mtr"},
    {{4, 13}, "Zoom"},  {{4, 14}, "LoopRec"},   {{5, 10}, "ConC"},
    {{5, 11}, "Page>"}, {{5, 13}, "ScrlBack"},  {{5, 14}, "QuikPnch"},
    {{6, 10}, "ConD"},  {{6, 11}, "MasterByp"}, {{6, 13}, "ScrollFwd"},
    {{6, 14}, "RTZ"},   {{7, 10}, "ConE"},      {{7, 11}, "ESC"},
    {{7, 13}, "ViewUP"}, {{7, 14}, "REW"},      {{8, 10}, "mod_shift"},
    {{8, 11}, "DispMode"}, {{8, 13}, "ViewDown"}, {{8, 14}, "FWD"},
    {{9, 10}, "mod_option"}, {{9, 14}, "STOP"},
};

// EQ mode: OSC address + 7-char LCD label per encoder (parallel arrays).
const std::array<std::string, 8> kEqParams = {
    "/fxeq/hipass/freq", "/fxeq/loshelf/gain", "/fxeq/band/1/freq",
    "/fxeq/band/1/gain", "/fxeq/band/2/freq",  "/fxeq/band/2/gain",
    "/fxeq/hishelf/gain", "/fxeq/lopass/freq"};
const std::array<std::string, 8> kEqNames = {
    "HP Freq", "LoShf G", "B1 Freq", "B1 Gain",
    "B2 Freq", "B2 Gain", "HiShf G", "LP Freq"};

// press -> Reaper action command id
const std::map<std::string, int> kActions = {
    {"RTZ", 40042}, {"Undo", 40029},  {"WMix", 40078},  {"MemLoc", 40157},
    {"WEdit", 40153}, {"ESC", 41074}, {"MasterByp", 8}, {"WPlugin", 41749},
    {"MstrFadrs", 40075}};

std::string label_for(int note, int subid) {
    auto it = kButtons.find({note, subid});
    return it == kButtons.end() ? std::string() : it->second;
}

bool code_for(const std::string& name, int& note, int& subid) {
    for (const auto& [k, v] : kButtons)
        if (v == name) { note = k.first; subid = k.second; return true; }
    return false;
}

std::vector<std::string> split_path(const char* path) {
    std::vector<std::string> out;
    std::string tok;
    std::istringstream is(path);
    while (std::getline(is, tok, '/'))
        if (!tok.empty()) out.push_back(tok);
    return out;
}

float arg_as_float(const char* types, lo_arg** argv, int i) {
    switch (types[i]) {
        case 'f': return argv[i]->f;
        case 'i': return static_cast<float>(argv[i]->i);
        case 'd': return static_cast<float>(argv[i]->d);
        case 'T': return 1.0f;
        case 'F': return 0.0f;
        default:  return 0.0f;
    }
}

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

void osc_err(int num, const char* msg, const char* where) {
    std::fprintf(stderr, "osc error %d: %s (%s)\n", num, msg ? msg : "",
                 where ? where : "");
}

// NAV[arrow][mode] -> (address, behavior). behavior: 'h' hold, 'p' pulse, 't' tap.
struct NavAct { std::string addr; char behavior; };
NavAct nav_lookup(const std::string& arrow, int nav) {
    // nav: 0 None, 1 Bank, 2 Nudge, 3 Zoom
    if (arrow == "ScrlBack") {
        if (nav == 3) return {"/zoom/x/-", 'p'};
        if (nav == 1) return {"/device/track/bank/-", 't'};
        if (nav == 2) return {"/device/track/-", 't'};
        return {"/scroll/x/-", 'h'};
    }
    if (arrow == "ScrollFwd") {
        if (nav == 3) return {"/zoom/x/+", 'p'};
        if (nav == 1) return {"/device/track/bank/+", 't'};
        if (nav == 2) return {"/device/track/+", 't'};
        return {"/scroll/x/+", 'h'};
    }
    if (arrow == "ViewUP") {
        if (nav == 3) return {"/zoom/y/+", 'p'};
        return {"/scroll/y/-", 'h'};
    }
    // ViewDown
    if (nav == 3) return {"/zoom/y/-", 'p'};
    return {"/scroll/y/+", 'h'};
}

}  // namespace

ReaperBackend::ReaperBackend(std::string host, std::string send_port,
                             std::string recv_port)
    : host_(std::move(host)),
      send_port_(std::move(send_port)),
      recv_port_(std::move(recv_port)) {
    pan_.fill(0.5); vol_.fill(0.0); send_.fill(0.0); fxp_.fill(0.5); eq_.fill(0.5);
    tx_ = lo_address_new(host_.c_str(), send_port_.c_str());
    rx_ = lo_server_thread_new(recv_port_.c_str(), osc_err);
    if (rx_) {
        lo_server_thread_add_method(rx_, nullptr, nullptr, osc_cb, this);
        lo_server_thread_start(rx_);
    }
}

void ReaperBackend::stop() {
    if (rx_) { lo_server_thread_free(rx_); rx_ = nullptr; }   // joins the recv thread
}

ReaperBackend::~ReaperBackend() {
    stop();
    if (tx_) { lo_address_free(tx_); tx_ = nullptr; }
}

void ReaperBackend::send_f(const std::string& path, float v) {
    if (tx_) lo_send(tx_, path.c_str(), "f", v);
}
void ReaperBackend::send_i(const std::string& path, int v) {
    if (tx_) lo_send(tx_, path.c_str(), "i", v);
}

const char* ReaperBackend::enc_suffix() const {
    return enc_mode_ == Enc::Send ? "send/1/volume" : "pan";
}

double ReaperBackend::active_value(int s) const {
    switch (enc_mode_) {
        case Enc::EQ:   return eq_[s];
        case Enc::Insert:
        case Enc::Dyn:  return fxp_[s];
        case Enc::Send: return send_[s];
        default:        return pan_[s];
    }
}

void ReaperBackend::set_active(int s, double v) {
    switch (enc_mode_) {
        case Enc::EQ:   eq_[s] = v; break;
        case Enc::Insert:
        case Enc::Dyn:  fxp_[s] = v; break;
        case Enc::Send: send_[s] = v; break;
        default:        pan_[s] = v; break;
    }
}

std::string ReaperBackend::fmt_pan(double v) const {
    int p = static_cast<int>(std::lround((v - 0.5) * 200));
    if (p == 0) return "C";
    return (p < 0 ? "L" : "R") + std::to_string(std::abs(p));
}
std::string ReaperBackend::fmt_pct(double v) const {
    return std::to_string(static_cast<int>(std::lround(v * 100))) + "%";
}
std::string ReaperBackend::fmt_active(int s) const {
    return enc_mode_ == Enc::Pan ? fmt_pan(pan_[s]) : fmt_pct(active_value(s));
}
std::string ReaperBackend::param_name(int e) const {
    if (enc_mode_ == Enc::EQ) return kEqNames[e];
    if (enc_mode_ == Enc::Insert || enc_mode_ == Enc::Dyn) {
        return fxname_[e].empty() ? ("Par " + std::to_string(e + 1)) : fxname_[e];
    }
    return "";
}

void ReaperBackend::value_cell(int s) {
    if (!fb_) return;
    if (is_fx()) fb_->lcd_channel(s, fmt_pct(active_value(s)));   // bottom row
    else         fb_->lcd_status(s, fmt_active(s));               // top row
}
void ReaperBackend::label_cell(int s) {
    if (!fb_) return;
    if (is_fx()) fb_->lcd_status(s, param_name(s));               // top row
    else         fb_->lcd_channel(s, names_[s]);                  // bottom row
}
void ReaperBackend::repaint() {
    for (int s = 0; s < 8; ++s) { label_cell(s); value_cell(s); }
}
void ReaperBackend::active_ring(int e, double v) {
    if (!fb_) return;
    if (enc_mode_ == Enc::Pan) fb_->ring_dot(e, v);
    else                       fb_->ring_fill(e, v);
}

void ReaperBackend::led(const std::string& name, bool on) {
    int note, subid;
    if (fb_ && code_for(name, note, subid))
        fb_->strip_led(static_cast<uint8_t>(note), subid, on);
}
void ReaperBackend::enc_mode_leds() {
    led("Pan", enc_mode_ == Enc::Pan);
    led("Send", enc_mode_ == Enc::Send);
    led("Insert", enc_mode_ == Enc::Insert);
    led("EQ", enc_mode_ == Enc::EQ);
    led("Dynamics", enc_mode_ == Enc::Dyn);
}
void ReaperBackend::nav_mode_leds() {
    led("Bank", nav_mode_ == Nav::Bank);
    led("Nudge", nav_mode_ == Nav::Nudge);
    led("Zoom", nav_mode_ == Nav::Zoom);
}
void ReaperBackend::update_recsel_led() {
    bool any = std::any_of(selected_.begin(), selected_.end(),
                           [&](int s) { return recarm_.count(s) != 0; });
    led("RecSel", any);
}

void ReaperBackend::set_enc_mode(Enc m) {
    enc_mode_ = m;
    if (std::getenv("COMMAND8_DEBUG"))
        std::fprintf(stderr, "[mode] -> %d (fx=%d)\n", static_cast<int>(m), is_fx());
    if (m == Enc::Insert) send_f("/device/fx/follows/lasttouched", 1.0f);
    else if (m == Enc::Dyn) {
        send_f("/device/fx/follows/device", 1.0f);
        send_i("/device/fx/select", DYN_FX_SLOT);
    }
    enc_mode_leds();
    repaint();
}

void ReaperBackend::handle_nav(const std::string& arrow, bool pressed) {
    const int nav = nav_mode_ == Nav::Bank ? 1
                    : nav_mode_ == Nav::Nudge ? 2
                    : nav_mode_ == Nav::Zoom ? 3 : 0;
    NavAct a = nav_lookup(arrow, nav);
    if (a.behavior == 'h') { send_f(a.addr, pressed ? 1.0f : 0.0f); return; }
    if (!pressed) return;
    if (a.behavior == 'p') { send_f(a.addr, 1.0f); send_f(a.addr, 0.0f); }
    else                     send_f(a.addr, 1.0f);   // tap
}

void ReaperBackend::on_start() {
    // Cold-start defaults so the surface isn't blank before Reaper's first
    // feedback burst (it only re-sends track names on connect/refresh). Any
    // real feedback that arrived first is preserved (only empty names filled).
    std::lock_guard<std::mutex> lk(m_);
    for (int s = 0; s < 8; ++s)
        if (names_[s].empty()) names_[s] = "Trk " + std::to_string(s + 1);
    enc_mode_leds();
    nav_mode_leds();
    led("Flip", flip_);
    repaint();
    for (int s = 0; s < 8; ++s) active_ring(s, pan_[s]);   // centre dots
    std::printf("Reaper backend ready (OSC send %s:%s, recv :%s).\n",
                host_.c_str(), send_port_.c_str(), recv_port_.c_str());
}

void ReaperBackend::on_fader(int strip, double v) {
    std::lock_guard<std::mutex> lk(m_);
    const std::string n = std::to_string(strip + 1);
    if (flipped()) {
        set_active(strip, v);
        send_f("/track/" + n + "/" + enc_suffix(), static_cast<float>(v));
        value_cell(strip);
        active_ring(strip, v);
    } else {
        vol_[strip] = v;
        send_f("/track/" + n + "/volume", static_cast<float>(v));
        if (!is_fx() && fb_) {
            fb_->lcd_channel(strip, fmt_pct(v));   // Channel-Data: show the level
            if (disp_hold_) {                       // held: persistent, no revert
                flash_pending_[strip] = false;
            } else {                                // else: revert to name shortly
                flash_pending_[strip] = true;
                flash_deadline_[strip] = std::chrono::steady_clock::now() + FLASH_MS;
            }
        }
    }
}

void ReaperBackend::tick() {
    std::lock_guard<std::mutex> lk(m_);
    if (!fb_) return;
    const auto now = std::chrono::steady_clock::now();
    for (int s = 0; s < 8; ++s) {
        if (!flash_pending_[s] || now < flash_deadline_[s]) continue;
        flash_pending_[s] = false;
        if (!is_fx() && !disp_hold_) fb_->lcd_channel(s, names_[s]);   // revert
    }
}

void ReaperBackend::on_encoder(int strip, int delta) {
    std::lock_guard<std::mutex> lk(m_);
    const double step = held_mods_.empty() ? PAN_STEP : PAN_STEP_FINE;
    const std::string n = std::to_string(strip + 1);

    if (enc_mode_ == Enc::Insert || enc_mode_ == Enc::Dyn) {
        double nv = clamp01(fxp_[strip] + delta * step); fxp_[strip] = nv;
        send_f("/fxparam/" + n + "/value", static_cast<float>(nv));
        value_cell(strip); active_ring(strip, nv);
        return;
    }
    if (enc_mode_ == Enc::EQ) {
        double nv = clamp01(eq_[strip] + delta * step); eq_[strip] = nv;
        send_f(kEqParams[strip], static_cast<float>(nv));
        value_cell(strip); active_ring(strip, nv);
        return;
    }
    if (flipped()) {   // encoder drives volume
        double nv = clamp01(vol_[strip] + delta * step); vol_[strip] = nv;
        send_f("/track/" + n + "/volume", static_cast<float>(nv));
        return;
    }
    double nv = clamp01(active_value(strip) + delta * step);
    set_active(strip, nv);
    send_f("/track/" + n + "/" + enc_suffix(), static_cast<float>(nv));
    value_cell(strip); active_ring(strip, nv);
}

void ReaperBackend::on_select(int strip, bool pressed) {
    if (pressed) { std::lock_guard<std::mutex> lk(m_);
        send_f("/track/" + std::to_string(strip + 1) + "/select/toggle", 1.0f); }
}
void ReaperBackend::on_mute(int strip, bool pressed) {
    if (pressed) { std::lock_guard<std::mutex> lk(m_);
        send_f("/track/" + std::to_string(strip + 1) + "/mute/toggle", 1.0f); }
}
void ReaperBackend::on_solo(int strip, bool pressed) {
    if (pressed) { std::lock_guard<std::mutex> lk(m_);
        send_f("/track/" + std::to_string(strip + 1) + "/solo/toggle", 1.0f); }
}

void ReaperBackend::on_button(uint8_t note, uint8_t subid, bool pressed) {
    std::lock_guard<std::mutex> lk(m_);
    const std::string name = label_for(note, subid);
    if (std::getenv("COMMAND8_DEBUG"))
        std::fprintf(stderr, "[btn] (%u,%u) '%s' %s\n", note, subid,
                     name.c_str(), pressed ? "down" : "up");
    if (name.empty()) return;

    if (name.rfind("mod_", 0) == 0) {
        if (pressed) held_mods_.insert(name); else held_mods_.erase(name);
        return;
    }
    if (name == "Flip") {
        if (pressed) { flip_ = !flip_; led("Flip", flip_); }
        return;
    }
    if (name == "DispMode") {
        disp_hold_ = pressed && !is_fx();
        if (is_fx() || !fb_) return;
        if (pressed) for (int s = 0; s < 8; ++s) fb_->lcd_channel(s, fmt_pct(vol_[s]));
        else         for (int s = 0; s < 8; ++s) fb_->lcd_channel(s, names_[s]);
        return;
    }
    if (name == "Pan")      { if (pressed) set_enc_mode(Enc::Pan);    return; }
    if (name == "Send")     { if (pressed) set_enc_mode(Enc::Send);   return; }
    if (name == "Insert")   { if (pressed) set_enc_mode(Enc::Insert); return; }
    if (name == "EQ")       { if (pressed) set_enc_mode(Enc::EQ);     return; }
    if (name == "Dynamics") { if (pressed) set_enc_mode(Enc::Dyn);    return; }

    if (name == "RecSel") {
        if (pressed)
            for (int s : selected_)
                send_f("/track/" + std::to_string(s + 1) + "/recarm/toggle", 1.0f);
        return;
    }
    if (name == "REW") { send_f("/rewind", pressed ? 1.0f : 0.0f); return; }
    if (name == "FWD") { send_f("/forward", pressed ? 1.0f : 0.0f); return; }

    if (name == "Bank" || name == "Nudge" || name == "Zoom") {
        if (pressed) {
            Nav want = name == "Bank" ? Nav::Bank : name == "Nudge" ? Nav::Nudge : Nav::Zoom;
            nav_mode_ = (nav_mode_ == want) ? Nav::None : want;
            nav_mode_leds();
        }
        return;
    }
    if (name == "ScrlBack" || name == "ScrollFwd" || name == "ViewUP" || name == "ViewDown") {
        handle_nav(name, pressed);
        return;
    }

    if (!pressed) return;
    if (name == "Play")          { send_f("/play", 1.0f); return; }
    if (name == "STOP")          { send_f("/stop", 1.0f); return; }
    if (name == "LoopPlay")      { send_f("/repeat", 1.0f); return; }
    auto ai = kActions.find(name);
    if (ai != kActions.end()) {
        int cid = ai->second;
        if (name == "Undo" && !held_mods_.empty()) cid = 40030;   // shift = redo
        send_f("/action/" + std::to_string(cid), 1.0f);
    }
}

int ReaperBackend::osc_cb(const char* path, const char* types, lo_arg** argv,
                          int argc, lo_message, void* user) {
    static_cast<ReaperBackend*>(user)->handle(path, types, argv, argc);
    return 0;
}

void ReaperBackend::handle(const char* path, const char* types, lo_arg** argv,
                           int argc) {
    if (!fb_) return;
    std::lock_guard<std::mutex> lk(m_);
    const auto t = split_path(path);
    if (t.empty()) return;

    // focused-FX param value (Insert/Dynamics): /fxparam/<i>/value
    if (t.size() == 3 && t[0] == "fxparam" && t[2] == "value") {
        int i = std::atoi(t[1].c_str()) - 1;
        if (i < 0 || i > 7) return;
        fxp_[i] = argc ? arg_as_float(types, argv, 0) : 0.f;
        if (enc_mode_ == Enc::Insert || enc_mode_ == Enc::Dyn) {
            fb_->ring_fill(i, fxp_[i]); value_cell(i);
        }
        return;
    }
    // focused-FX param name: /fxparam/<i>/name
    if (t.size() == 3 && t[0] == "fxparam" && t[2] == "name") {
        int i = std::atoi(t[1].c_str()) - 1;
        if (i < 0 || i > 7) return;
        fxname_[i] = (argc && types[0] == 's') ? &argv[0]->s : "";
        if (enc_mode_ == Enc::Insert || enc_mode_ == Enc::Dyn) label_cell(i);
        return;
    }
    // ReaEQ feedback: /fxeq/...
    if (t[0] == "fxeq") {
        for (int e = 0; e < 8; ++e) {
            if (kEqParams[e] == path) {
                eq_[e] = argc ? arg_as_float(types, argv, 0) : 0.f;
                if (enc_mode_ == Enc::EQ) { fb_->ring_fill(e, eq_[e]); value_cell(e); }
                return;
            }
        }
        return;
    }
    // transport LEDs
    if (t.size() == 1) {
        float v = argc ? arg_as_float(types, argv, 0) : 0.f;
        if (t[0] == "play")        led("Play", v != 0);
        else if (t[0] == "stop")   led("STOP", v != 0);
        else if (t[0] == "repeat") led("LoopPlay", v != 0);
        return;
    }
    // per-track feedback
    if (t[0] == "track" && t.size() >= 3) {
        int s = std::atoi(t[1].c_str()) - 1;
        if (s < 0 || s > 7) return;
        std::string suffix = t[2];
        for (size_t i = 3; i < t.size(); ++i) suffix += "/" + t[i];

        if (suffix == "name") {
            names_[s] = (argc && types[0] == 's') ? &argv[0]->s : "";
            if (!disp_hold_ && !is_fx()) fb_->lcd_channel(s, names_[s]);
            return;
        }
        float v = argc ? arg_as_float(types, argv, 0) : 0.f;
        if (suffix == "volume") {
            vol_[s] = v;
            if (flipped()) fb_->ring_fill(s, v); else fb_->fader(s, v);
            if (!is_fx() && disp_hold_) fb_->lcd_channel(s, fmt_pct(v));
        } else if (suffix == "pan") {
            pan_[s] = v;
            if (enc_mode_ == Enc::Pan) {
                if (flipped()) fb_->fader(s, v); else fb_->ring_dot(s, v);
                value_cell(s);
            }
        } else if (suffix == "send/1/volume") {
            send_[s] = v;
            if (enc_mode_ == Enc::Send) {
                if (flipped()) fb_->fader(s, v); else fb_->ring_fill(s, v);
                value_cell(s);
            }
        } else if (suffix == "mute") {
            fb_->mute_led(s, v != 0);
        } else if (suffix == "solo") {
            fb_->solo_led(s, v != 0);
        } else if (suffix == "select") {
            if (v != 0) selected_.insert(s); else selected_.erase(s);
            fb_->select_led(s, v != 0); update_recsel_led();
        } else if (suffix == "recarm") {
            if (v != 0) recarm_.insert(s); else recarm_.erase(s);
            fb_->recarm_led(s, v != 0); update_recsel_led();
        } else if (suffix == "monitor") {
            fb_->surface_led(s, v != 0);
        } else if (suffix == "vu") {
            fb_->meter(s, v);
        }
    }
}

}  // namespace command8
