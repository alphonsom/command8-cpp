// SPDX-License-Identifier: GPL-3.0-or-later
// command8-mackie: present the Command|8 to the DAW as a Mackie Control on a
// virtual MIDI port. Use a snd-virmidi kernel port so DAWs (e.g. Bitwig) can
// see it:  sudo modprobe snd-virmidi  -> "Virtual Raw MIDI 4-0..4-3".
// Then point the DAW's Mackie Control support at that port.
//
//   ./command8-mackie [--mcu-port VirMIDI]
//
// Waits for the Command|8 at startup and re-opens on replug.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "controller.hpp"
#include "mackie/mackie_backend.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
command8::Surface* g_surface = nullptr;
void on_sig(int) { g_stop = 1; if (g_surface) g_surface->stop(); }
void nap(int seconds) {
    for (int i = 0; i < seconds * 10 && !g_stop; ++i) {
        timespec ts{0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
}
}  // namespace

int main(int argc, char** argv) {
    std::string mcu_port = "VirMIDI";
    for (int i = 1; i < argc - 1; ++i)
        if (!std::strcmp(argv[i], "--mcu-port")) mcu_port = argv[++i];

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    command8::MackieBackend backend(mcu_port);
    if (!backend.ok()) {
        std::fprintf(stderr, "Could not open an MCU port matching '%s'. Load "
                     "snd-virmidi (sudo modprobe snd-virmidi) and retry.\n",
                     mcu_port.c_str());
        return 1;
    }
    std::printf("command8-mackie: MCU up on %s; waiting for the Command|8...\n",
                mcu_port.c_str());

    while (!g_stop) {
        command8::Surface surface;
        g_surface = &surface;
        if (!surface.open()) { g_surface = nullptr; nap(2); continue; }
        command8::Controller controller(surface, backend);   // attaches Feedback
        controller.run();                                    // blocks
        backend.stop();                                      // stop rx before fb dies
        surface.close();
        g_surface = nullptr;
        if (!g_stop) std::printf("command8-mackie: device removed; waiting...\n");
    }
    std::printf("\nbye\n");
    return 0;
}
