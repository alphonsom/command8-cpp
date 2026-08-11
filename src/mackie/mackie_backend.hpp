// SPDX-License-Identifier: GPL-3.0-or-later
// Mackie Control (MCU) emulation backend. Presents the Command|8 to the DAW as
// a Mackie Control on a MIDI port: on Linux typically a snd-virmidi "Virtual
// Raw MIDI" kernel port, on Windows a Windows MIDI Services loopback pair.
//
// This class owns no translation logic of its own. It is an adapter around
// src/mcu/c8_mcu.c -- the same freestanding C module the dongle firmware runs,
// and the one the unit tests cover. Keeping a second copy here would mean the
// host bridge and the dongle could drift apart silently, each correct against
// its own tests and different on the wire.
//
// The adapter costs one re-encode. Surface decodes device bytes into Events,
// Controller hands those to the on_* methods below, and they rebuild the
// original bytes for the C module. That round trip is exact -- every field
// survives, and a test pins it -- but it is a round trip, and it exists only
// to keep the Backend abstraction intact. A byte-level path from Surface
// straight into the translator would be cleaner and is the obvious next
// refactor; it was not done here because it touches every Surface backend.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "backend.hpp"
#include "protocol.hpp"
#include "mcu/c8_mcu.h"
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

// --- Event -> wire bytes ---------------------------------------------------
//
// Controller hands back-ends decoded Events, but the translator works in
// Command|8 wire bytes, so these rebuild what the device originally sent. The
// round trip must be lossless or control values would shift, so they are inline
// and free rather than private members: tests/test_mackie_roundtrip.cpp feeds
// every possible input through decode_* and back and asserts the bytes match.

// Fader: the 10-bit position is split across the CC number (low 3 bits) and its
// data byte (upper 7), which is how the surface transmits it.
inline void surface_bytes_for_fader(int strip, double value01, uint8_t out[3]) {
    int v10 = static_cast<int>(value01 * 1023.0 + 0.5);
    if (v10 < 0) v10 = 0;
    if (v10 > 1023) v10 = 1023;
    out[0] = 0xB0;
    out[1] = static_cast<uint8_t>(((v10 & 0x07) << 3) | (strip & STRIP_MASK));
    out[2] = static_cast<uint8_t>((v10 >> 3) & 0x7F);
}

inline void surface_bytes_for_encoder(int strip, int delta, uint8_t out[3]) {
    out[0] = 0xB0;
    out[1] = static_cast<uint8_t>(ENCODER_CC_BASE + (strip & STRIP_MASK));
    out[2] = delta > 0 ? ENC_RIGHT : ENC_LEFT;
}

// Buttons: velocity carries the sub-id in the low 6 bits, bit 6 = pressed.
inline void surface_bytes_for_button(uint8_t note, uint8_t subid, bool pressed,
                                    uint8_t out[3]) {
    out[0] = 0x90;
    out[1] = note;
    out[2] = static_cast<uint8_t>((pressed ? VEL_ON : 0) | (subid & SUBID_MASK));
}

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

    // Drives meter ballistics inside the translator (~10 Hz from Controller).
    void tick() override;

private:
    // Rebuild the Command|8 wire bytes an Event came from and hand them to the
    // translator. See the class comment on why this round trip exists.
    void feed_surface(uint8_t status, uint8_t d1, uint8_t d2);

    // Translator output. Static trampolines because the C module takes plain
    // function pointers; `user` is always `this`.
    static void to_daw(void* user, const uint8_t* msg, size_t len);
    static void to_surface(void* user, const uint8_t* msg, size_t len);

    c8_mcu_t mcu_{};
    std::unique_ptr<MidiPort> port_;
};

}  // namespace command8
