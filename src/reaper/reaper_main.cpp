// SPDX-License-Identifier: GPL-3.0-or-later
// command8-reaper: bridge the Command|8 to Reaper over OSC. Load
// reaper/Command8.ReaperOSC (shipped in this repo) as the Reaper OSC pattern
// and set the device to receive on 8000 / send to 9000.
//
// Hotplug: waits for the device at startup and exits the run loop when it is
// removed, then re-opens on replug (so it self-heals; a systemd user service
// with Restart=always also covers process crashes).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "controller.hpp"
#include "reaper/reaper_backend.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
command8::Surface* g_surface = nullptr;
void on_sig(int) { g_stop = 1; if (g_surface) g_surface->stop(); }

void sleep_interruptible(int seconds) {
    for (int i = 0; i < seconds * 10 && !g_stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", send_port = "8000", recv_port = "9000";
    std::string port_match = command8::kDefaultPortMatch;
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--osc-host")) host = argv[++i];
        else if (!std::strcmp(argv[i], "--osc-send-port")) send_port = argv[++i];
        else if (!std::strcmp(argv[i], "--osc-recv-port")) recv_port = argv[++i];
        else if (!std::strcmp(argv[i], "--port")) port_match = argv[++i];
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    std::printf("command8-reaper: waiting for the Command|8...\n");

    while (!g_stop) {
        auto surface = command8::make_surface();
        g_surface = surface.get();
        if (!surface->open(port_match)) {      // device absent -> wait and retry
            g_surface = nullptr;
            sleep_interruptible(2);
            continue;
        }
        // Backend (and its OSC server) live only while the device is connected.
        command8::ReaperBackend backend(host, send_port, recv_port);
        command8::Controller controller(*surface, backend);
        controller.run();                      // blocks until stop or removal
        backend.stop();                        // join the OSC thread while the
                                               // Feedback handle is still alive
        surface->close();
        g_surface = nullptr;
        if (!g_stop) std::printf("command8-reaper: device removed; waiting...\n");
    }
    std::printf("\nbye\n");
    return 0;
}
