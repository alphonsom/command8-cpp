// SPDX-License-Identifier: GPL-3.0-or-later
// High-level, normalized feedback API over a Surface. Back-ends work in 0..1
// terms and named LEDs; this class handles the device-specific bit-packing
// (meter fills bottom-up, pan shows a single dot, rings fill proportionally).
#pragma once

#include <string>

#include "surface.hpp"

namespace command8 {

class Feedback {
public:
    static constexpr int RING_LEDS = 11;   // LEDs per encoder ring
    static constexpr int METER_ROWS = 6;   // LED rows per strip meter

    explicit Feedback(Surface& surface) : s_(surface) {}

    void fader(int strip, double value01);     // motor fader
    void meter(int strip, double value01);     // fills from the bottom LED up
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

private:
    Surface& s_;
};

}  // namespace command8
