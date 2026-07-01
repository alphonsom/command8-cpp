// SPDX-License-Identifier: GPL-3.0-or-later
// Framework-free unit tests for the protocol decode/encode layer.
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "protocol.hpp"

using namespace command8;

static int g_fail = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                  \
        }                                                              \
    } while (0)

static bool eq(const std::vector<uint8_t>& a, std::initializer_list<int> b) {
    if (a.size() != b.size()) return false;
    size_t i = 0;
    for (int x : b)
        if (a[i++] != static_cast<uint8_t>(x)) return false;
    return true;
}

int main() {
    // --- decode: note-on ---
    CHECK(std::holds_alternative<HeartbeatEvent>(decode_note_on(0, 127)));
    {
        auto e = decode_note_on(0, 0x40 | 3);
        auto* b = std::get_if<ButtonEvent>(&e);
        CHECK(b && b->category == "select" && b->subid == 3 && b->pressed);
    }
    {
        auto e = decode_note_on(0, 3);   // release (bit6 clear)
        auto* b = std::get_if<ButtonEvent>(&e);
        CHECK(b && b->category == "select" && !b->pressed);
    }
    {
        auto e = decode_note_on(2, 0x40 | 1);
        auto* b = std::get_if<ButtonEvent>(&e);
        CHECK(b && b->category == "solo" && b->subid == 1 && b->pressed);
    }
    {
        auto e = decode_note_on(3, 0x42);
        auto* b = std::get_if<ButtonEvent>(&e);
        CHECK(b && b->category == "mute" && b->subid == 2 && b->pressed);
    }
    {
        auto e = decode_note_on(5, 0x40 | 4);   // fader touch
        auto* t = std::get_if<FaderTouchEvent>(&e);
        CHECK(t && t->fader == 4 && t->touched);
    }
    {
        auto e = decode_note_on(0, 0x40 | 10);   // discrete button (subid > 7)
        auto* b = std::get_if<ButtonEvent>(&e);
        CHECK(b && b->category == "btn_0_10" && b->subid == 10 && b->pressed);
    }

    // --- decode: control change ---
    {
        auto e = decode_cc(64, 0x41);
        auto* x = std::get_if<EncoderEvent>(&e);
        CHECK(x && x->encoder == 0 && x->delta == 1);
    }
    {
        auto e = decode_cc(71, 0x3F);
        auto* x = std::get_if<EncoderEvent>(&e);
        CHECK(x && x->encoder == 7 && x->delta == -1);
    }
    {
        auto e = decode_cc(9, 100);   // fader = 9&7 = 1; value10 = (100<<3)|1
        auto* f = std::get_if<FaderMoveEvent>(&e);
        CHECK(f && f->fader == 1 && f->value == 100 && f->value10 == 801);
    }
    CHECK(std::holds_alternative<std::monostate>(decode_cc(100, 0)));

    // --- encode: feedback ---
    CHECK(eq(heartbeat(), {0x90, 0, 127}));
    CHECK(eq(button_led(3, 2, true), {0x90, 3, 0x42}));
    CHECK(eq(button_led(3, 2, false), {0x90, 3, 2}));
    CHECK(eq(fader_position(1, 100), {0xB0, 1, 100}));
    CHECK(eq(meter(2, 0x0F), {0x90, 0x4F, 2}));
    CHECK(eq(encoder_ring(0, 0x08), {0xF0, 0x13, 0x01, 0x00, 0x00, 0x08, 0xF7}));
    CHECK(eq(encoder_ring(0, 0x100), {0xF0, 0x13, 0x01, 0x00, 0x10, 0x00, 0xF7}));
    CHECK(eq(lcd_channel(0, "Rhodes"),
             {0xF0, 0x13, 0x01, 0x40, 0x10, 0x7F, 0x00,
              'R', 'h', 'o', 'd', 'e', 's', ' ', 0xF7}));
    CHECK(eq(lcd_status(0, "12345678"),   // truncated to 7 chars
             {0xF0, 0x13, 0x01, 0x40, 0x00, 0x7F, 0x00,
              '1', '2', '3', '4', '5', '6', '7', 0xF7}));
    CHECK(eq(select_surface_led(3, true), {0x90, 0, 0x43}));
    CHECK(eq(select_surface_led(0, false), {0x90, 0, 0}));

    if (g_fail) { std::printf("%d checks FAILED\n", g_fail); return 1; }
    std::printf("all protocol tests passed\n");
    return 0;
}
