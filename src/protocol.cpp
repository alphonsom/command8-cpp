// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"

namespace command8 {

namespace {
std::vector<uint8_t> note_on(uint8_t note, uint8_t vel) {
    return {0x90, note, vel};   // channel 1 note-on
}
}  // namespace

Event decode_note_on(uint8_t note, uint8_t vel) {
    if (note == HEARTBEAT_NOTE && vel == HEARTBEAT_VEL)
        return HeartbeatEvent{};

    const uint8_t subid = vel & SUBID_MASK;
    const bool pressed = (vel & VEL_ON) != 0;

    // subid 0-7 = strip-indexed control (note picks the function); higher subids
    // on the same note are discrete buttons keyed on (note, subid).
    if (subid <= STRIP_SUBID_MAX) {
        if (note == NOTE_FADER_TOUCH)
            return FaderTouchEvent{subid, pressed};
        switch (note) {
            case NOTE_SELECT: return ButtonEvent{note, "select", subid, pressed};
            case NOTE_SOLO:   return ButtonEvent{note, "solo",   subid, pressed};
            case NOTE_MUTE:   return ButtonEvent{note, "mute",   subid, pressed};
            default: break;
        }
    }
    return ButtonEvent{note,
                       "btn_" + std::to_string(note) + "_" + std::to_string(subid),
                       subid, pressed};
}

Event decode_cc(uint8_t cc, uint8_t val) {
    if (cc >= ENCODER_CC_BASE && cc < ENCODER_CC_BASE + ENCODER_CC_COUNT) {
        const int delta = (val == ENC_RIGHT) ? 1 : (val == ENC_LEFT) ? -1 : 0;
        return EncoderEvent{static_cast<uint8_t>(cc - ENCODER_CC_BASE), delta};
    }
    if (cc <= FADER_CC_MAX) {
        const uint8_t fader = cc & STRIP_MASK;
        const uint8_t hi = (cc >> 3) & STRIP_MASK;       // 3 extra low-order position bits
        const uint16_t value10 = (static_cast<uint16_t>(val) << 3) | hi;
        return FaderMoveEvent{fader, val, value10};
    }
    return std::monostate{};
}

std::vector<uint8_t> heartbeat() {
    return note_on(HEARTBEAT_NOTE, HEARTBEAT_VEL);
}

std::vector<uint8_t> button_led(uint8_t note, uint8_t subid, bool on) {
    const uint8_t s = subid & SUBID_MASK;
    return note_on(note, on ? (VEL_ON | s) : s);
}

std::vector<uint8_t> fader_position(uint8_t fader, uint8_t value) {
    return {0xB0, static_cast<uint8_t>(fader & STRIP_MASK),
            static_cast<uint8_t>(value & 0x7F)};
}

std::vector<uint8_t> meter(uint8_t strip, uint8_t level_bits) {
    return note_on(static_cast<uint8_t>(METER_NOTE_BASE | (level_bits & 0x3F)),
                   static_cast<uint8_t>(strip & STRIP_MASK));
}

std::vector<uint8_t> encoder_ring(uint8_t encoder, uint16_t led_bits) {
    // low 7 bits -> LEDs 1-7 (leds byte); bits 7-10 -> LEDs 8-11 packed into enc_addr.
    const uint8_t enc_addr =
        (encoder & STRIP_MASK) | (((led_bits >> 7) << 3) & 0x78);
    const uint8_t leds = led_bits & 0x7F;
    return {0xF0, DIGIDESIGN_MFR, RING_DEVICE, RING_CMD, enc_addr, leds, 0xF7};
}

std::vector<uint8_t> lcd(uint8_t cell, const std::string& text) {
    std::vector<uint8_t> m = {0xF0, DIGIDESIGN_MFR, RING_DEVICE, LCD_CMD,
                              static_cast<uint8_t>(cell & 0x7F), 0x7F, 0x00};
    std::string t = text.substr(0, LCD_CELL_WIDTH);
    t.resize(LCD_CELL_WIDTH, ' ');
    for (char c : t) {
        const auto b = static_cast<uint8_t>(c);
        m.push_back((b >= 0x20 && b <= 0x7E) ? b : 0x20);   // printable ASCII or space
    }
    m.push_back(0xF7);
    return m;
}

std::vector<uint8_t> lcd_channel(uint8_t strip, const std::string& text) {
    return lcd(static_cast<uint8_t>(LCD_CHANNEL_CELL_BASE + (strip & STRIP_MASK)), text);
}

std::vector<uint8_t> lcd_status(uint8_t strip, const std::string& text) {
    return lcd(static_cast<uint8_t>(LCD_STATUS_CELL_BASE + (strip & STRIP_MASK)), text);
}

std::vector<uint8_t> select_surface_led(uint8_t strip, bool on) {
    const uint8_t s = strip & STRIP_MASK;
    return note_on(NOTE_SELECT_SURFACE_LED, on ? (VEL_ON | s) : s);
}

}  // namespace command8
