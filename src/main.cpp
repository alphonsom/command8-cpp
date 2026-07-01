// SPDX-License-Identifier: GPL-3.0-or-later
// Proof-of-concept: open the Command|8, do the handshake, and print decoded
// input events. A brief LED "hello" confirms the output path works too.
#include <csignal>
#include <cstdio>

#include "surface.hpp"

namespace {
command8::Surface* g_surface = nullptr;

void on_sigint(int) {
    if (g_surface) g_surface->stop();
}

// helper for std::visit
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
}  // namespace

int main() {
    command8::Surface surface;
    g_surface = &surface;

    if (!surface.open()) {
        std::fprintf(stderr, "Could not open the Command|8 (is it connected and the "
                             "snd-usb-audio quirk loaded?)\n");
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    surface.set_callback([](const command8::Event& ev) {
        std::visit(overloaded{
            [](const command8::HeartbeatEvent&) {},
            [](const command8::ButtonEvent& b) {
                std::printf("button   %-10s note=%2d subid=%2d %s\n",
                            b.category.c_str(), b.note, b.subid,
                            b.pressed ? "down" : "up");
            },
            [](const command8::FaderTouchEvent& f) {
                std::printf("touch    fader=%d %s\n", f.fader, f.touched ? "on" : "off");
            },
            [](const command8::FaderMoveEvent& f) {
                std::printf("fader    %d = %3d (10-bit %4d)\n", f.fader, f.value, f.value10);
            },
            [](const command8::EncoderEvent& e) {
                std::printf("encoder  %d delta %+d\n", e.encoder, e.delta);
            },
            [](std::monostate) {},
        }, ev);
    });

    // quick output test: sweep the green select LEDs on, then off
    for (uint8_t s = 0; s < 8; ++s) surface.send(command8::button_led(command8::NOTE_SELECT_GREEN_LED, s, true));
    for (uint8_t s = 0; s < 8; ++s) surface.send(command8::button_led(command8::NOTE_SELECT_GREEN_LED, s, false));

    std::printf("Command|8 open. Move faders / turn encoders / press buttons. Ctrl-C to quit.\n");
    surface.run();
    surface.close();
    std::printf("\nbye\n");
    return 0;
}
