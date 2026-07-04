// SPDX-License-Identifier: GPL-3.0-or-later
// Demo front-end built on the Controller/Backend/Feedback abstraction. It has no
// DAW: it prints normalized input and drives loopback feedback so you can see the
// engine working — faders move the meters, encoders move a pan dot, and
// select/mute/solo toggle their LEDs. A real integration (Reaper, etc.) would be
// a Backend just like this one.
//
//   ./command8-monitor [--port <name-substring>] [--list]
//
// --list prints the MIDI ports the backend can see and exits (use it to check
// that the Command|8's input shows up at all).
#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "controller.hpp"

namespace {
command8::Surface* g_surface = nullptr;
void on_sigint(int) { if (g_surface) g_surface->stop(); }
}  // namespace

class DemoBackend : public command8::Backend {
public:
    void on_start() override {
        for (int s = 0; s < 8; ++s) {
            fb_->lcd_channel(s, "Ch " + std::to_string(s + 1));
            fb_->ring_dot(s, pan_[s]);
        }
        std::printf("Demo: faders->meters, encoders->pan dot, select/mute/solo toggle LEDs.\n");
    }
    void on_fader(int strip, double v) override {
        fb_->meter(strip, v);
        std::printf("fader   %d = %.2f\n", strip, v);
    }
    void on_encoder(int strip, int delta) override {
        pan_[strip] = std::min(1.0, std::max(0.0, pan_[strip] + delta * 0.05));
        fb_->ring_dot(strip, pan_[strip]);
        std::printf("encoder %d -> pan %.2f\n", strip, pan_[strip]);
    }
    void on_select(int strip, bool pressed) override {
        if (!pressed) return;
        sel_[strip] = !sel_[strip];
        fb_->select_led(strip, sel_[strip]);
    }
    void on_mute(int strip, bool pressed) override {
        if (!pressed) return;
        mute_[strip] = !mute_[strip];
        fb_->mute_led(strip, mute_[strip]);
    }
    void on_solo(int strip, bool pressed) override {
        if (!pressed) return;
        solo_[strip] = !solo_[strip];
        fb_->solo_led(strip, solo_[strip]);
    }
    void on_button(uint8_t note, uint8_t subid, bool pressed) override {
        if (pressed) std::printf("button  note=%d subid=%d\n", note, subid);
    }

private:
    std::array<double, 8> pan_{{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}};
    std::array<bool, 8> sel_{}, mute_{}, solo_{};
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // live output when piped/logged
    std::string port_match = command8::kDefaultPortMatch;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) {
            command8::print_midi_ports();
            return 0;
        }
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port_match = argv[++i];
    }

    auto surface = command8::make_surface();
    g_surface = surface.get();
    if (!surface->open(port_match)) {
        std::fprintf(stderr, "Could not open the Command|8 (connected? on Linux: "
                             "snd-usb-audio quirk loaded?)\n");
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    DemoBackend backend;
    command8::Controller controller(*surface, backend);
    std::printf("Command|8 open. Ctrl-C to quit.\n");
    controller.run();
    surface->close();
    std::printf("\nbye\n");
    return 0;
}
