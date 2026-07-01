// SPDX-License-Identifier: GPL-3.0-or-later
#include "reaper/reaper_backend.hpp"

#include <algorithm>
#include <cstdio>
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

std::string label_for(int note, int subid) {
    auto it = kButtons.find({note, subid});
    return it == kButtons.end() ? std::string() : it->second;
}

bool code_for(const std::string& name, int& note, int& subid) {
    for (const auto& [k, v] : kButtons) {
        if (v == name) { note = k.first; subid = k.second; return true; }
    }
    return false;
}

std::vector<std::string> split_path(const char* path) {
    std::vector<std::string> out;
    std::string s(path), tok;
    std::istringstream is(s);
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

void osc_err(int num, const char* msg, const char* where) {
    std::fprintf(stderr, "osc error %d: %s (%s)\n", num, msg ? msg : "",
                 where ? where : "");
}

}  // namespace

ReaperBackend::ReaperBackend(std::string host, std::string send_port,
                             std::string recv_port)
    : host_(std::move(host)),
      send_port_(std::move(send_port)),
      recv_port_(std::move(recv_port)) {
    pan_.fill(0.5);
    tx_ = lo_address_new(host_.c_str(), send_port_.c_str());
    rx_ = lo_server_thread_new(recv_port_.c_str(), osc_err);
    if (rx_) {
        lo_server_thread_add_method(rx_, nullptr, nullptr, osc_cb, this);
        lo_server_thread_start(rx_);
    }
}

ReaperBackend::~ReaperBackend() {
    if (rx_) { lo_server_thread_free(rx_); rx_ = nullptr; }
    if (tx_) { lo_address_free(tx_); tx_ = nullptr; }
}

void ReaperBackend::send_f(const std::string& path, float v) {
    if (tx_) lo_send(tx_, path.c_str(), "f", v);
}

void ReaperBackend::on_start() {
    std::printf("Reaper backend ready (OSC send %s:%s, recv :%s).\n",
                host_.c_str(), send_port_.c_str(), recv_port_.c_str());
}

void ReaperBackend::on_fader(int strip, double v) {
    send_f("/track/" + std::to_string(strip + 1) + "/volume", static_cast<float>(v));
}

void ReaperBackend::on_encoder(int strip, int delta) {
    // Reaper has no relative pan over OSC: track absolute value and send it.
    double nv = std::min(1.0, std::max(0.0, pan_[strip] + delta * PAN_STEP));
    pan_[strip] = nv;
    send_f("/track/" + std::to_string(strip + 1) + "/pan", static_cast<float>(nv));
    if (fb_) fb_->ring_dot(strip, nv);   // Reaper won't echo our own change
}

void ReaperBackend::on_select(int strip, bool pressed) {
    if (pressed) send_f("/track/" + std::to_string(strip + 1) + "/select/toggle", 1.0f);
}

void ReaperBackend::on_mute(int strip, bool pressed) {
    if (pressed) send_f("/track/" + std::to_string(strip + 1) + "/mute/toggle", 1.0f);
}

void ReaperBackend::on_solo(int strip, bool pressed) {
    if (pressed) send_f("/track/" + std::to_string(strip + 1) + "/solo/toggle", 1.0f);
}

void ReaperBackend::on_button(uint8_t note, uint8_t subid, bool pressed) {
    const std::string name = label_for(note, subid);
    if (name.empty() || name.rfind("mod_", 0) == 0) return;   // ignore/modifier

    if (name == "RecSel") {   // arm the selected track(s)
        if (!pressed) return;
        std::lock_guard<std::mutex> lk(m_);
        for (int s : selected_)
            send_f("/track/" + std::to_string(s + 1) + "/recarm/toggle", 1.0f);
        return;
    }
    if (!pressed) return;
    if (name == "Play")          send_f("/play", 1.0f);
    else if (name == "STOP")     send_f("/stop", 1.0f);
    else if (name == "LoopPlay") send_f("/repeat", 1.0f);
}

void ReaperBackend::led_for(const std::string& name, bool on) {
    int note, subid;
    if (fb_ && code_for(name, note, subid))
        fb_->strip_led(static_cast<uint8_t>(note), subid, on);
}

void ReaperBackend::update_recsel_led() {
    bool any;
    {
        std::lock_guard<std::mutex> lk(m_);
        any = std::any_of(selected_.begin(), selected_.end(),
                          [&](int s) { return recarm_.count(s) != 0; });
    }
    led_for("RecSel", any);
}

int ReaperBackend::osc_cb(const char* path, const char* types, lo_arg** argv,
                          int argc, lo_message, void* user) {
    static_cast<ReaperBackend*>(user)->handle(path, types, argv, argc);
    return 0;
}

void ReaperBackend::handle(const char* path, const char* types, lo_arg** argv,
                           int argc) {
    if (!fb_) return;
    const auto t = split_path(path);
    if (t.empty()) return;

    if (t[0] == "track" && t.size() >= 3) {
        const int strip = std::atoi(t[1].c_str()) - 1;
        if (strip < 0 || strip > 7) return;
        std::string suffix = t[2];
        for (size_t i = 3; i < t.size(); ++i) suffix += "/" + t[i];

        if (suffix == "name") {
            std::string nm = (argc > 0 && types[0] == 's') ? &argv[0]->s : "";
            fb_->lcd_channel(strip, nm);
            return;
        }
        const float v = argc > 0 ? arg_as_float(types, argv, 0) : 0.0f;
        if (suffix == "volume")      fb_->fader(strip, v);
        else if (suffix == "pan")  { pan_[strip] = v; fb_->ring_dot(strip, v); }
        else if (suffix == "mute")   fb_->mute_led(strip, v != 0);
        else if (suffix == "solo")   fb_->solo_led(strip, v != 0);
        else if (suffix == "select") {
            { std::lock_guard<std::mutex> lk(m_);
              if (v != 0) selected_.insert(strip); else selected_.erase(strip); }
            fb_->select_led(strip, v != 0);
            update_recsel_led();
        } else if (suffix == "recarm") {
            { std::lock_guard<std::mutex> lk(m_);
              if (v != 0) recarm_.insert(strip); else recarm_.erase(strip); }
            fb_->recarm_led(strip, v != 0);
            update_recsel_led();
        } else if (suffix == "monitor") {
            fb_->surface_led(strip, v != 0);
        } else if (suffix == "vu") {
            fb_->meter(strip, v);
        }
        return;
    }

    if (t.size() == 1) {   // transport LEDs
        const float v = argc > 0 ? arg_as_float(types, argv, 0) : 0.0f;
        if (t[0] == "play")        led_for("Play", v != 0);
        else if (t[0] == "stop")   led_for("STOP", v != 0);
        else if (t[0] == "repeat") led_for("LoopPlay", v != 0);
    }
}

}  // namespace command8
