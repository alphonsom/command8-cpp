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

int Feedback::rows_to_bits(double rows) {
    // Round rather than truncate: truncation loses the top row (11/12 of full
    // scale showed 5 of 6 LEDs). Any non-zero signal lights at least one LED,
    // so quiet material is distinguishable from silence.
    if (rows < 0.05) return 0;
    const int n = clampi(static_cast<int>(std::lround(rows)), 1, METER_ROWS);
    // Fill from the high bits down so a low signal lights the BOTTOM LED (the
    // meter is addressed top-to-bottom).
    return ((1 << n) - 1) << (METER_ROWS - n);
}

double Feedback::decayed_locked(int strip, std::chrono::steady_clock::time_point now) {
    double cur = meter_level_[strip];
    if (meter_decay_ms_ > 0 && cur > 0.0) {
        const auto t = meter_t_[strip];
        if (t.time_since_epoch().count() != 0) {
            const double dt =
                std::chrono::duration<double>(now - t).count();
            cur = std::max(0.0, cur - dt * (METER_ROWS * 1000.0 / meter_decay_ms_));
        }
    }
    return cur;
}

void Feedback::meter(int strip, double v) {
    if (strip < 0 || strip >= STRIPS) return;
    const double rows = std::max(0.0, std::min<double>(METER_ROWS, v * METER_ROWS));
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(meter_mutex_);
    // The host value is a PEAK: rise to it instantly, then let tick() decay it.
    meter_level_[strip] = meter_decay_ms_ > 0
                              ? std::max(decayed_locked(strip, now), rows)
                              : rows;
    meter_t_[strip] = now;
}

void Feedback::tick() {
    const auto now = std::chrono::steady_clock::now();
    int due_strip[STRIPS];
    int due_bits[STRIPS];
    int n_due = 0;

    {
        std::lock_guard<std::mutex> lock(meter_mutex_);
        for (int i = 0; i < STRIPS; ++i) {
            const double cur = decayed_locked(i, now);
            meter_level_[i] = cur;
            meter_t_[i] = now;
            const int bits = rows_to_bits(cur);
            if (meter_sent_[i] && bits == meter_bits_[i]) continue;   // unchanged
            meter_bits_[i] = bits;
            meter_sent_[i] = true;
            due_strip[n_due] = i;
            due_bits[n_due] = bits;
            ++n_due;
        }
    }
    // Send outside the lock: s_.send() blocks on USB.
    for (int i = 0; i < n_due; ++i)
        s_.send(command8::meter(static_cast<uint8_t>(due_strip[i]),
                                static_cast<uint8_t>(due_bits[i])));
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

void Feedback::raw(const uint8_t* bytes, size_t n) {
    s_.send(std::vector<uint8_t>(bytes, bytes + n));
}

}  // namespace command8
