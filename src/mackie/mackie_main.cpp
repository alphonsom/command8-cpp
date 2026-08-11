// SPDX-License-Identifier: GPL-3.0-or-later
// command8-mackie: present the Command|8 to the DAW as a Mackie Control on a
// virtual MIDI port, then point the DAW's Mackie Control support at it.
//
// Linux: use a snd-virmidi kernel port so DAWs (e.g. Bitwig) can see it:
//   sudo modprobe snd-virmidi   -> "Virtual Raw MIDI 4-0..4-3"
// Windows: create a Windows MIDI Services loopback pair once:
//   midi loopback create --name-a "Command8 MCU Bridge" --name-b "Command8 MCU DAW"
// The bridge takes the "Bridge" end; point the DAW's Mackie Control at the
// "DAW" end (in + out). Leave the "Command|8 Bridge" device ports themselves
// disabled in the DAW -- those belong to this process, and a DAW holding them
// stops it opening the surface at all.
//
//   ./command8-mackie [--mcu-port <match>] [--mcu-recv <match>]
//                     [--mcu-send <match>] [--port <device-match>]
//
// --mcu-port sets both directions (the default duplex layout on both
// platforms); --mcu-recv/--mcu-send override each direction separately (e.g.
// a two-cable loopMIDI setup). Waits for the Command|8 at startup and
// re-opens on replug.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "controller.hpp"
#include "mackie/mackie_backend.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
command8::Surface* g_surface = nullptr;
void on_sig(int) { g_stop = 1; if (g_surface) g_surface->stop(); }
void nap(int seconds) {
    for (int i = 0; i < seconds * 10 && !g_stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}  // namespace

int main(int argc, char** argv) {
    std::string recv_match = command8::kDefaultMcuRecvMatch;
    std::string send_match = command8::kDefaultMcuSendMatch;
    std::string port_match = command8::kDefaultPortMatch;
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--mcu-port")) { recv_match = send_match = argv[++i]; }
        else if (!std::strcmp(argv[i], "--mcu-recv")) recv_match = argv[++i];
        else if (!std::strcmp(argv[i], "--mcu-send")) send_match = argv[++i];
        else if (!std::strcmp(argv[i], "--port")) port_match = argv[++i];
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    std::unique_ptr<command8::Surface> surface;

#ifdef __APPLE__
    // Claim the Command|8 *before* touching RtMidi/CoreMIDI (constructed below
    // via MackieBackend's MidiPort). Once a CoreMIDI client has ever existed
    // in this process, this process's own future libusb claims of the device
    // race CoreMIDI's in-process device-notification handling for the same
    // interface and lose -- so this ordering only buys the very first claim.
    // It does NOT make replug recovery below sudo-free: once MackieBackend
    // constructs its RtMidi client, a claim lost to a later unplug cannot be
    // regained without root from this process, even if that client is torn
    // down first (verified: tearing down and rebuilding the CoreMIDI client
    // around the reclaim attempt does not help, and the failure does not
    // clear with time -- it is a standing condition for the rest of this
    // process's life, not a transient race). A full fix would need a
    // privilege-separated helper process to hold the libusb claim, which is
    // more than this ordering trick can give us; for now, treat first launch
    // as sudo-free and replug-while-running as still requiring it.
    // Linux/Windows don't share this problem at all (their MidiPort never
    // touches the physical device), so they keep the original order below:
    // fail fast on a missing MCU port without first blocking on hardware that
    // may not even be plugged in yet.
    std::printf("command8-mackie: waiting for the Command|8...\n");
    surface = command8::make_surface();
    g_surface = surface.get();
    while (!g_stop && !surface->open(port_match)) nap(2);
    if (g_stop) { std::printf("\nbye\n"); return 0; }
#endif

    command8::MackieBackend backend(recv_match, send_match);
    if (!backend.ok()) {
#ifdef _WIN32
        std::fprintf(stderr, "Could not open the MCU ports ('%s' / '%s'). Create "
                     "the loopback pair first:\n  midi loopback create --name-a "
                     "\"Command8 MCU Bridge\" --name-b \"Command8 MCU DAW\"\n"
                     "(or create cables in loopMIDI) and retry.\n",
                     recv_match.c_str(), send_match.c_str());
#else
        std::fprintf(stderr, "Could not open an MCU port matching '%s'. Load "
                     "snd-virmidi (sudo modprobe snd-virmidi) and retry.\n",
                     recv_match.c_str());
#endif
        return 1;
    }

#ifdef __APPLE__
    std::printf("command8-mackie: MCU up.\n");
#else
    std::printf("command8-mackie: MCU up; waiting for the Command|8...\n");
    surface = command8::make_surface();
    g_surface = surface.get();
    while (!g_stop && !surface->open(port_match)) nap(2);
    if (g_stop) { std::printf("\nbye\n"); return 0; }
#endif

    while (!g_stop) {
        command8::Controller controller(*surface, backend);  // attaches Feedback
        controller.run();                                    // blocks
        backend.stop();                                      // stop rx before fb dies
        surface->close();
        if (g_stop) break;
        std::printf("command8-mackie: device removed; waiting...\n");
        surface = command8::make_surface();
        g_surface = surface.get();
        while (!g_stop && !surface->open(port_match)) nap(2);
    }
    g_surface = nullptr;
    std::printf("\nbye\n");
    return 0;
}
