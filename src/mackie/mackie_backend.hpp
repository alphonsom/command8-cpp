// SPDX-License-Identifier: GPL-3.0-or-later
// Mackie Control (MCU) emulation backend. Presents the Command|8 to the DAW as
// a Mackie Control on a MIDI port: on Linux typically a snd-virmidi "Virtual
// Raw MIDI" kernel port, on Windows a pair of loopMIDI cables. Built on
// libcommand8 with a MidiPort for the MCU side, so the translation logic is
// transport-agnostic.
//
// Translation ported from the original Python driver's mackie profile.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "backend.hpp"
#include "midi_port.hpp"

namespace command8 {

// Default MCU port-name matches. recv = DAW-to-bridge, send = bridge-to-DAW.
// Linux: one duplex virmidi port carries both directions. Windows: the bridge
// end of a Windows MIDI Services loopback pair (create once with
//   midi loopback create --name-a "Command8 MCU Bridge" --name-b "Command8 MCU DAW"
// ); the bridge opens its own end both ways and the DAW's Mackie Control uses
// the other, so neither hears its own output. macOS: these are the names of the
// virtual ports the bridge CREATES rather than ones to search for, so no
// loopback is needed and the DAW points at this name for both directions.
//
// The ends are named for who owns them because "A" and "B" gave no clue which
// was which, and picking the wrong one in the DAW produces a silent failure
// that looks exactly like broken hardware.
//
// Do NOT put a bar in these names. "Command|8" is the surface port matcher (see
// kDefaultPortMatch in surface.hpp), and it takes the first prefix match it
// finds -- a loopback called "Command|8 MCU ..." could win that match ahead of
// the real device depending on enumeration order. The unbarred "Command8" is
// what keeps the two families of port distinguishable.
#if defined(_WIN32)
inline constexpr const char* kDefaultMcuRecvMatch = "Command8 MCU Bridge";
inline constexpr const char* kDefaultMcuSendMatch = "Command8 MCU Bridge";
#elif defined(__APPLE__)
inline constexpr const char* kDefaultMcuRecvMatch = "Command|8";
inline constexpr const char* kDefaultMcuSendMatch = "Command|8";
#else
inline constexpr const char* kDefaultMcuRecvMatch = "VirMIDI";
inline constexpr const char* kDefaultMcuSendMatch = "VirMIDI";
#endif

class MackieBackend : public Backend {
public:
    explicit MackieBackend(std::string recv_match = kDefaultMcuRecvMatch,
                           std::string send_match = kDefaultMcuSendMatch);
    ~MackieBackend() override;

    bool ok() const { return port_ && port_->ok(); }   // MCU port opened?

    void on_start() override;                      // starts the MCU receive loop
    void stop();                                   // stop rx before Feedback dies

    void on_fader(int strip, double value01) override;
    void on_encoder(int strip, int delta) override;
    void on_select(int strip, bool pressed) override;
    void on_mute(int strip, bool pressed) override;
    void on_solo(int strip, bool pressed) override;
    void on_button(uint8_t note, uint8_t subid, bool pressed) override;

private:
    void send_note(int note, bool on);
    void send_cc(int cc, int value);
    void send_pitch(int channel, int value);
    void handle_mcu(const std::vector<uint8_t>& m);
    void lcd_sysex(const uint8_t* data, int len);
    void set_nav_mode(int mode);   // Bank/Nudge/Zoom radio group
    void paint_nav_leds();

    int nav_mode_ = 0;               // 0=Bank,1=Nudge,2=Zoom (input thread only)
    std::unique_ptr<MidiPort> port_;

    std::mutex state_m_;              // guards selected_
    std::set<int> selected_;
    std::array<uint8_t, 112> lcd_{};  // Mackie 2x56 LCD buffer
};

}  // namespace command8
