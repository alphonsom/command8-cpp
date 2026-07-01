// SPDX-License-Identifier: GPL-3.0-or-later
// command8-reaper: bridge the Command|8 to Reaper over OSC. Load
// reaper/Command8.ReaperOSC (from command8-linux) as the Reaper OSC pattern and
// set the device to receive on 8000 / send to 9000.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "controller.hpp"
#include "reaper/reaper_backend.hpp"

namespace {
command8::Surface* g_surface = nullptr;
void on_sigint(int) { if (g_surface) g_surface->stop(); }
}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", send_port = "8000", recv_port = "9000";
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--osc-host")) host = argv[++i];
        else if (!std::strcmp(argv[i], "--osc-send-port")) send_port = argv[++i];
        else if (!std::strcmp(argv[i], "--osc-recv-port")) recv_port = argv[++i];
    }

    command8::Surface surface;
    g_surface = &surface;
    if (!surface.open()) {
        std::fprintf(stderr, "Could not open the Command|8 (connected? quirk loaded?)\n");
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    command8::ReaperBackend backend(host, send_port, recv_port);
    command8::Controller controller(surface, backend);
    std::printf("command8-reaper running. Ctrl-C to quit.\n");
    controller.run();
    surface.close();
    std::printf("\nbye\n");
    return 0;
}
