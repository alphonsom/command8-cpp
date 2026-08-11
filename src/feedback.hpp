// SPDX-License-Identifier: GPL-3.0-or-later
// High-level, normalized feedback API over a Surface. Back-ends work in 0..1
// terms and named LEDs; this class handles the device-specific bit-packing
// (meter fills bottom-up, pan shows a single dot, rings fill proportionally).
#pragma once

#include <array>
#include <chrono>
#include <mutex>
#include <string>

#include "surface.hpp"

namespace command8 {

class Feedback {
public:
    static constexpr int RING_LEDS = 11;   // LEDs per encoder ring
    static constexpr int METER_ROWS = 6;   // LED rows per strip meter
    static constexpr int STRIPS = 8;

    // Meter falloff: time for a full-scale meter to reach zero. Hosts send
    // meter values sparsely (Reaper transmits only when the quantised 0-12
    // level changes - about 1 Hz in practice) and expect the surface to supply
    // the ballistics in between, the way real MCU hardware does in firmware.
    // Without this the display freezes on the last value and never falls when
    // playback stops. 0 disables it and follows the host exactly.
    static constexpr int DEFAULT_METER_DECAY_MS = 1200;

    explicit Feedback(Surface& surface) : s_(surface) {}

    void set_meter_decay_ms(int ms) { meter_decay_ms_ = ms; }

    // Advance meter falloff and push any changed rows. Safe to call often;
    // Controller drives it from the Surface tick.
    void tick();

    void fader(int strip, double value01);     // motor fader
    void meter(int strip, double value01);     // peak in, ballistics out
    void ring_fill(int strip, double value01); // thermometer (level-like params)
    void ring_dot(int strip, double value01);  // single dot (pan position)

    void select_led(int strip, bool on);
    void mute_led(int strip, bool on);
    void solo_led(int strip, bool on);
    void recarm_led(int strip, bool on);
    void surface_led(int strip, bool on);      // LED in the select button surface
    void strip_led(uint8_t note, int strip, bool on);  // generic strip LED

    void lcd_channel(int strip, const std::string& text);  // bottom row
    void lcd_status(int strip, const std::string& text);   // top row

    // Pass device-ready bytes straight through. For back-ends that do their own
    // encoding -- specifically MackieBackend, which delegates to the shared C
    // translator in src/mcu/. That module already emits Command|8 wire format,
    // so re-deriving it through the calls above would mean decoding its output
    // only to encode it again. Everything else should use the named methods.
    void raw(const uint8_t* bytes, size_t n);

private:
    // Level after decay since the last advance. Caller holds meter_mutex_.
    double decayed_locked(int strip, std::chrono::steady_clock::time_point now);
    static int rows_to_bits(double rows);

    Surface& s_;

    int meter_decay_ms_ = DEFAULT_METER_DECAY_MS;
    std::mutex meter_mutex_;   // meter() runs on the host's rx thread, tick() on the surface thread
    std::array<double, STRIPS> meter_level_{};                 // current, in rows
    std::array<int, STRIPS> meter_bits_{};                     // last bitfield sent
    std::array<bool, STRIPS> meter_sent_{};                    // has meter_bits_ been sent yet
    std::array<std::chrono::steady_clock::time_point, STRIPS> meter_t_{};
};

}  // namespace command8
