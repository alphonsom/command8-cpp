// SPDX-License-Identifier: GPL-3.0-or-later
#include "feedback.hpp"

#include <algorithm>
#include <cmath>

namespace command8 {

namespace {
int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace

void Feedback::fader(int strip, double v) {
    const int val = clampi(static_cast<int>(std::lround(v * 127.0)), 0, 127);
    s_.send(fader_position(static_cast<uint8_t>(strip), static_cast<uint8_t>(val)));
}

void Feedback::meter(int strip, double v) {
    // rows lit from the value; fill from the high bits down so a low signal
    // lights the BOTTOM LED (the meter is addressed top-to-bottom).
    const int rows = clampi(static_cast<int>(v * METER_ROWS), 0, METER_ROWS);
    const int bits = ((1 << rows) - 1) << (METER_ROWS - rows);
    s_.send(command8::meter(static_cast<uint8_t>(strip), static_cast<uint8_t>(bits)));
}

void Feedback::ring_fill(int strip, double v) {
    const int pos = clampi(static_cast<int>(std::lround(v * RING_LEDS)), 0, RING_LEDS);
    const int bits = (1 << pos) - 1;
    s_.send(encoder_ring(static_cast<uint8_t>(strip), static_cast<uint16_t>(bits)));
}

void Feedback::ring_dot(int strip, double v) {
    const int pos = clampi(static_cast<int>(std::lround(v * (RING_LEDS - 1))), 0, RING_LEDS - 1);
    s_.send(encoder_ring(static_cast<uint8_t>(strip), static_cast<uint16_t>(1 << pos)));
}

void Feedback::strip_led(uint8_t note, int strip, bool on) {
    s_.send(button_led(note, static_cast<uint8_t>(strip), on));
}

void Feedback::select_led(int strip, bool on) { strip_led(NOTE_SELECT_GREEN_LED, strip, on); }
void Feedback::mute_led(int strip, bool on)   { strip_led(NOTE_MUTE, strip, on); }
void Feedback::solo_led(int strip, bool on)   { strip_led(NOTE_SOLO, strip, on); }
void Feedback::recarm_led(int strip, bool on) { strip_led(NOTE_RECARM_LED, strip, on); }

void Feedback::surface_led(int strip, bool on) {
    s_.send(select_surface_led(static_cast<uint8_t>(strip), on));
}

void Feedback::lcd_channel(int strip, const std::string& text) {
    s_.send(command8::lcd_channel(static_cast<uint8_t>(strip), text));
}

void Feedback::lcd_status(int strip, const std::string& text) {
    s_.send(command8::lcd_status(static_cast<uint8_t>(strip), text));
}

}  // namespace command8
