// SPDX-License-Identifier: GPL-3.0-or-later
// Digidesign Command|8 native MIDI protocol: decode (input) + encode (feedback).
// See docs/PROTOCOL.md for the reverse-engineering evidence. DAW-agnostic: this
// layer knows nothing about OSC or any specific host application.
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace command8 {

// --- protocol constants ----------------------------------------------------
inline constexpr uint8_t HEARTBEAT_NOTE = 0;    // note 0 vel 127 = online heartbeat / wake
inline constexpr uint8_t HEARTBEAT_VEL  = 127;
inline constexpr uint8_t VEL_ON     = 0x40;     // velocity bit 6 = pressed / LED-on
inline constexpr uint8_t SUBID_MASK = 0x3F;     // velocity low 6 bits = sub-id
inline constexpr uint8_t STRIP_MASK = 0x07;     // strip index 0-7
inline constexpr uint8_t STRIP_SUBID_MAX = 7;   // subid <=7 -> strip-indexed control

// input note categories (strip-indexed; subid = strip)
inline constexpr uint8_t NOTE_SELECT      = 0;  // select press (also heartbeat at vel 127)
inline constexpr uint8_t NOTE_SOLO        = 2;
inline constexpr uint8_t NOTE_MUTE        = 3;
inline constexpr uint8_t NOTE_FADER_TOUCH = 5;

// output LED notes (may differ from the input note for the same control)
inline constexpr uint8_t NOTE_SELECT_GREEN_LED   = 1;
inline constexpr uint8_t NOTE_RECARM_LED         = 4;
inline constexpr uint8_t NOTE_SELECT_SURFACE_LED = 0;  // LED in the select button surface

// encoders / faders
inline constexpr uint8_t ENCODER_CC_BASE  = 64;
inline constexpr uint8_t ENCODER_CC_COUNT = 8;
inline constexpr uint8_t ENC_RIGHT = 0x41;      // +1 detent
inline constexpr uint8_t ENC_LEFT  = 0x3F;      // -1 detent
inline constexpr uint8_t FADER_CC_MAX = 63;

inline constexpr uint8_t METER_NOTE_BASE = 64;  // note = 64|row_bits, vel = strip

// Digidesign SysEx (rings + LCD)
inline constexpr uint8_t DIGIDESIGN_MFR = 0x13;
inline constexpr uint8_t RING_DEVICE = 0x01;
inline constexpr uint8_t RING_CMD = 0x00;
inline constexpr uint8_t LCD_CMD  = 0x40;
inline constexpr uint8_t LCD_CELL_WIDTH = 7;
inline constexpr uint8_t LCD_CHANNEL_CELL_BASE = 0x10;  // bottom row (channel names)
inline constexpr uint8_t LCD_STATUS_CELL_BASE  = 0x00;  // top row

// --- decoded input events --------------------------------------------------
struct HeartbeatEvent {};
struct ButtonEvent    { uint8_t note; std::string category; uint8_t subid; bool pressed; };
struct FaderTouchEvent{ uint8_t fader; bool touched; };
struct FaderMoveEvent { uint8_t fader; uint8_t value; uint16_t value10; };
struct EncoderEvent   { uint8_t encoder; int delta; };

// std::monostate = "not a recognised control"
using Event = std::variant<std::monostate, HeartbeatEvent, ButtonEvent,
                           FaderTouchEvent, FaderMoveEvent, EncoderEvent>;

// Decode a channel-voice message from the surface.
Event decode_note_on(uint8_t note, uint8_t velocity);
Event decode_cc(uint8_t cc, uint8_t value);

// --- host -> surface feedback (returns a raw MIDI byte sequence) ------------
std::vector<uint8_t> heartbeat();
std::vector<uint8_t> button_led(uint8_t note, uint8_t subid, bool on);
std::vector<uint8_t> fader_position(uint8_t fader, uint8_t value);
std::vector<uint8_t> meter(uint8_t strip, uint8_t level_bits);
std::vector<uint8_t> encoder_ring(uint8_t encoder, uint16_t led_bits);
std::vector<uint8_t> lcd(uint8_t cell, const std::string& text);
std::vector<uint8_t> lcd_channel(uint8_t strip, const std::string& text);
std::vector<uint8_t> lcd_status(uint8_t strip, const std::string& text);
std::vector<uint8_t> select_surface_led(uint8_t strip, bool on);

}  // namespace command8
