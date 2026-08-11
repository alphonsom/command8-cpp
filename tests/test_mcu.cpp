// SPDX-License-Identifier: GPL-3.0-or-later
// Framework-free unit tests for the freestanding C translator in src/mcu/.
//
// This module is the one piece of the project that will run on bare metal
// inside the dongle, where a wrong byte is expensive to find: there is no
// debugger in the data path, because halting a core stops the bit-banged USB
// host and drops the bus. So the contract is pinned here instead, on the
// desktop, and the firmware only ever ships code that passed.
//
// Two kinds of check:
//   1. Behaviour  -- exact bytes emitted for a given input.
//   2. Agreement  -- the C module's private copies of the Command|8 protocol
//                    constants still match protocol.hpp. The C module cannot
//                    include the C++ header, so drift is possible; this file
//                    includes both and makes drift a test failure.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "mcu/c8_mcu.h"
#include "protocol.hpp"

using namespace command8;

// MCU note-map bases, repeated here so the tests state the expected wire bytes
// independently of the module's own private constants.
static constexpr int N_REC_TEST = 0x00;

static int g_fail = 0;
#define CHECK(cond)                                                   \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

// --- capture harness -------------------------------------------------------

using Msg = std::vector<uint8_t>;

struct Capture {
    std::vector<Msg> daw;
    std::vector<Msg> surface;
    void clear() { daw.clear(); surface.clear(); }
};

static void cap_daw(void* u, const uint8_t* b, size_t n) {
    static_cast<Capture*>(u)->daw.emplace_back(b, b + n);
}
static void cap_surface(void* u, const uint8_t* b, size_t n) {
    static_cast<Capture*>(u)->surface.emplace_back(b, b + n);
}

struct Fixture {
    c8_mcu_t m{};
    Capture cap;
    Fixture() { c8_mcu_init(&m, cap_daw, cap_surface, &cap); }

    void from_surface(std::initializer_list<int> bytes) {
        std::vector<uint8_t> v;
        for (int b : bytes) v.push_back(static_cast<uint8_t>(b));
        c8_mcu_from_surface(&m, v.data(), v.size());
    }
    void from_daw(std::initializer_list<int> bytes) {
        std::vector<uint8_t> v;
        for (int b : bytes) v.push_back(static_cast<uint8_t>(b));
        c8_mcu_from_daw(&m, v.data(), v.size());
    }
};

static Msg msg(std::initializer_list<int> bytes) {
    Msg v;
    for (int b : bytes) v.push_back(static_cast<uint8_t>(b));
    return v;
}

static void dump(const char* what, const std::vector<Msg>& got) {
    std::printf("  %s: %zu message(s)\n", what, got.size());
    for (const auto& m : got) {
        std::printf("   ");
        for (uint8_t b : m) std::printf(" %02X", b);
        std::printf("\n");
    }
}

// Assert the captured messages are exactly `want`, printing both on mismatch.
static void expect(const char* what, const std::vector<Msg>& got,
                   const std::vector<Msg>& want, int line) {
    if (got == want) return;
    std::printf("FAIL %s:%d  %s\n", __FILE__, line, what);
    dump("got   ", got);
    dump("want  ", want);
    ++g_fail;
}
#define EXPECT_DAW(f, ...) expect("to DAW", (f).cap.daw, {__VA_ARGS__}, __LINE__)
#define EXPECT_SURFACE(f, ...) \
    expect("to surface", (f).cap.surface, {__VA_ARGS__}, __LINE__)

// --- 1. agreement with protocol.hpp ---------------------------------------
//
// The C module hardcodes these because it must build without any C++ header.
// If protocol.hpp ever changes one, this catches it at build time rather than
// on hardware.
static void test_constants_agree() {
    // Reproduce the C module's private values. Keep in step with c8_mcu.c.
    CHECK(HEARTBEAT_NOTE == 0);
    CHECK(HEARTBEAT_VEL == 127);
    CHECK(VEL_ON == 0x40);
    CHECK(SUBID_MASK == 0x3F);
    CHECK(STRIP_MASK == 0x07);
    CHECK(STRIP_SUBID_MAX == 7);
    CHECK(NOTE_SELECT == 0);
    CHECK(NOTE_SOLO == 2);
    CHECK(NOTE_MUTE == 3);
    CHECK(NOTE_FADER_TOUCH == 5);
    CHECK(NOTE_SELECT_GREEN_LED == 1);
    CHECK(NOTE_RECARM_LED == 4);
    CHECK(ENCODER_CC_BASE == 64);
    CHECK(ENCODER_CC_COUNT == 8);
    CHECK(ENC_RIGHT == 0x41);
    CHECK(ENC_LEFT == 0x3F);
    CHECK(FADER_CC_MAX == 63);
    CHECK(METER_NOTE_BASE == 64);
    CHECK(DIGIDESIGN_MFR == 0x13);
    CHECK(RING_DEVICE == 0x01);
    CHECK(RING_CMD == 0x00);
    CHECK(LCD_CMD == 0x40);
    CHECK(LCD_CELL_WIDTH == C8_MCU_LCD_WIDTH);
    CHECK(LCD_CHANNEL_CELL_BASE == 0x10);
    CHECK(LCD_STATUS_CELL_BASE == 0x00);
}

// The C module must emit byte-for-byte what protocol.cpp's encoders produce,
// since the surface is the same hardware either way.
static void test_encoders_match_protocol_cpp() {
    Fixture f;

    // Select LED on strip 3, driven by an MCU note-on.
    f.from_daw({0x90, 0x18 + 3, 0x7F});
    const auto want_led = button_led(NOTE_SELECT_GREEN_LED, 3, true);
    expect("select LED matches protocol.cpp", f.cap.surface, {want_led}, __LINE__);

    // Ring, single dot at pan centre.
    f.cap.clear();
    f.from_daw({0xB0, 0x30, 0x06});           // mode 0, pos 6 -> LED 5
    const auto want_ring = encoder_ring(0, 1 << 5);
    expect("ring dot matches protocol.cpp", f.cap.surface, {want_ring}, __LINE__);

    // Motor fader from a pitchbend.
    f.cap.clear();
    f.from_daw({0xE2, 0x00, 0x40});           // channel 2, 0x2000 = 8192
    const auto want_fader = fader_position(2, 64);
    expect("fader matches protocol.cpp", f.cap.surface, {want_fader}, __LINE__);
}

// --- 2. Command|8 -> MCU ---------------------------------------------------

static void test_heartbeat_is_swallowed() {
    Fixture f;
    // Note 0 velocity 127 is the heartbeat echo, not a Select press on strip
    // 63. Forwarding it would spray junk at the DAW four times a second.
    f.from_surface({0x90, 0, 127});
    EXPECT_DAW(f);
    EXPECT_SURFACE(f);
}

static void test_strip_buttons() {
    Fixture f;
    f.from_surface({0x90, NOTE_SELECT, VEL_ON | 5});   // Select 5 pressed
    EXPECT_DAW(f, msg({0x90, 0x18 + 5, 127}));

    f.cap.clear();
    f.from_surface({0x90, NOTE_SELECT, 5});            // released
    EXPECT_DAW(f, msg({0x90, 0x18 + 5, 0}));

    f.cap.clear();
    f.from_surface({0x90, NOTE_SOLO, VEL_ON | 2});
    EXPECT_DAW(f, msg({0x90, 0x08 + 2, 127}));

    f.cap.clear();
    f.from_surface({0x90, NOTE_MUTE, VEL_ON | 7});
    EXPECT_DAW(f, msg({0x90, 0x10 + 7, 127}));
}

static void test_fader_touch_not_forwarded() {
    Fixture f;
    f.from_surface({0x90, NOTE_FADER_TOUCH, VEL_ON | 0});
    EXPECT_DAW(f);
}

static void test_fader_scaling() {
    Fixture f;
    // Fader CC: low 3 bits of the CC carry extra position bits, so
    // cc = (hi << 3) | fader and value10 = (val << 3) | hi.
    // Bottom of travel -> pitchbend 0.
    f.from_surface({0xB0, 0x00, 0x00});
    EXPECT_DAW(f, msg({0xE0, 0x00, 0x00}));

    // Top of travel: val 127, hi 7 -> value10 1023 -> full scale 16383.
    f.cap.clear();
    f.from_surface({0xB0, (7 << 3) | 0, 0x7F});
    EXPECT_DAW(f, msg({0xE0, 0x7F, 0x7F}));

    // Fader 3 near mid travel: 512/1023 * 16383 = 8199.51, rounding to 8200.
    // Slightly above centre because 512 is just over half of 1023.
    f.cap.clear();
    f.from_surface({0xB0, (0 << 3) | 3, 0x40});   // val 64, hi 0 -> value10 512
    const int expect14 = 8200;
    EXPECT_DAW(f, msg({0xE3, expect14 & 0x7F, (expect14 >> 7) & 0x7F}));
}

// The integer fader conversion must round exactly as the double path did.
static void test_fader_rounding_matches_double() {
    for (int value10 = 0; value10 <= 1023; ++value10) {
        const int with_int = (value10 * 16383 * 2 + 1023) / 2046;
        const double v = value10 / 1023.0;
        const int with_double = static_cast<int>(std::lround(v * 16383.0));
        if (with_int != with_double) {
            std::printf("FAIL %s:%d  fader rounding differs at value10=%d "
                        "(int %d, double %d)\n",
                        __FILE__, __LINE__, value10, with_int, with_double);
            ++g_fail;
            return;
        }
    }
}

static void test_encoders() {
    Fixture f;
    f.from_surface({0xB0, ENCODER_CC_BASE + 2, ENC_RIGHT});
    EXPECT_DAW(f, msg({0xB0, 0x10 + 2, 1}));

    f.cap.clear();
    f.from_surface({0xB0, ENCODER_CC_BASE + 2, ENC_LEFT});
    EXPECT_DAW(f, msg({0xB0, 0x10 + 2, 0x41}));

    // A value that is neither detent is not a movement; guessing a direction
    // here would make the encoder drift.
    f.cap.clear();
    f.from_surface({0xB0, ENCODER_CC_BASE + 2, 0x00});
    EXPECT_DAW(f);
}

static void test_transport_buttons() {
    Fixture f;
    f.from_surface({0x90, 10, VEL_ON | 14});    // Play
    EXPECT_DAW(f, msg({0x90, 0x5E, 127}));

    f.cap.clear();
    f.from_surface({0x90, 9, VEL_ON | 14});     // Stop
    EXPECT_DAW(f, msg({0x90, 0x5D, 127}));

    f.cap.clear();
    f.from_surface({0x90, 42, VEL_ON | 20});    // unmapped: silence, not junk
    EXPECT_DAW(f);
}

// --- 3. navigation mode ----------------------------------------------------

static void test_nav_mode_retargets_arrows() {
    Fixture f;
    c8_mcu_start(&f.m);
    // Default is Bank: only the Bank LED lights.
    EXPECT_SURFACE(f, msg({0x90, 2, VEL_ON | 13}),
                      msg({0x90, 3, 13}),
                      msg({0x90, 4, 13}));

    // In Bank mode '>' is Bank Right.
    f.cap.clear();
    f.from_surface({0x90, 6, VEL_ON | 13});
    EXPECT_DAW(f, msg({0x90, 0x2F, 127}));

    // Switch to Nudge: LEDs repaint, no Zoom toggle (neither side is Zoom).
    f.cap.clear();
    f.from_surface({0x90, 3, VEL_ON | 13});
    EXPECT_DAW(f);
    EXPECT_SURFACE(f, msg({0x90, 2, 13}),
                      msg({0x90, 3, VEL_ON | 13}),
                      msg({0x90, 4, 13}));

    // Now '>' is Channel Right instead.
    f.cap.clear();
    f.from_surface({0x90, 6, VEL_ON | 13});
    EXPECT_DAW(f, msg({0x90, 0x31, 127}));

    // Entering Zoom pulses the DAW's Zoom modifier so the arrows zoom.
    f.cap.clear();
    f.from_surface({0x90, 4, VEL_ON | 13});
    EXPECT_DAW(f, msg({0x90, 0x64, 127}), msg({0x90, 0x64, 0}));

    // And '>' becomes cursor right.
    f.cap.clear();
    f.from_surface({0x90, 6, VEL_ON | 13});
    EXPECT_DAW(f, msg({0x90, 0x63, 127}));

    // Re-pressing the active mode is a no-op, not a second Zoom pulse.
    f.cap.clear();
    f.from_surface({0x90, 4, VEL_ON | 13});
    EXPECT_DAW(f);
    EXPECT_SURFACE(f);
}

static void test_recsel_arms_selected_tracks() {
    Fixture f;
    // Nothing selected: RecSel does nothing rather than arming everything.
    f.from_surface({0x90, 3, VEL_ON | 12});
    EXPECT_DAW(f);

    // DAW reports strips 1 and 4 selected.
    f.cap.clear();
    f.from_daw({0x90, 0x18 + 1, 0x7F});
    f.from_daw({0x90, 0x18 + 4, 0x7F});

    f.cap.clear();
    f.from_surface({0x90, 3, VEL_ON | 12});
    EXPECT_DAW(f, msg({0x90, N_REC_TEST + 1, 127}),
                  msg({0x90, N_REC_TEST + 4, 127}));

    // Deselecting strip 1 removes it from the arm set.
    f.from_daw({0x90, 0x18 + 1, 0x00});
    f.cap.clear();
    f.from_surface({0x90, 3, VEL_ON | 12});
    EXPECT_DAW(f, msg({0x90, N_REC_TEST + 4, 127}));
}

// --- 4. MCU -> Command|8 ---------------------------------------------------

static void test_led_feedback() {
    Fixture f;
    f.from_daw({0x90, 0x00 + 6, 0x7F});               // rec-arm 6 on
    EXPECT_SURFACE(f, msg({0x90, NOTE_RECARM_LED, VEL_ON | 6}));

    f.cap.clear();
    f.from_daw({0x80, 0x08 + 1, 0x00});               // note-off = solo 1 off
    EXPECT_SURFACE(f, msg({0x90, NOTE_SOLO, 1}));

    f.cap.clear();
    f.from_daw({0x90, 0x10 + 0, 0x00});               // velocity 0 = mute 0 off
    EXPECT_SURFACE(f, msg({0x90, NOTE_MUTE, 0}));

    f.cap.clear();
    f.from_daw({0x90, 0x5E, 0x7F});                   // Play LED
    EXPECT_SURFACE(f, msg({0x90, 10, VEL_ON | 14}));
}

static void test_ring_modes() {
    Fixture f;
    // pos 0 clears the ring whatever the mode nibble says.
    f.from_daw({0xB0, 0x30 + 1, 0x20});
    EXPECT_SURFACE(f, msg({0xF0, 0x13, 0x01, 0x00, 1, 0x00, 0xF7}));

    // mode 2 = wrap/fill: pos 4 lights LEDs 0..3.
    f.cap.clear();
    f.from_daw({0xB0, 0x30 + 1, 0x24});
    EXPECT_SURFACE(f, msg({0xF0, 0x13, 0x01, 0x00, 1, 0x0F, 0xF7}));

    // Fill past bit 7 spills into the address byte's high nibble.
    f.cap.clear();
    f.from_daw({0xB0, 0x30 + 0, 0x2B});               // pos 11 -> 11 LEDs lit
    const uint16_t bits = (1u << 11) - 1u;
    EXPECT_SURFACE(f, msg({0xF0, 0x13, 0x01, 0x00,
                           static_cast<int>((bits >> 7) << 3),
                           bits & 0x7F, 0xF7}));

    // mode 0 = single dot: pos 1 is the first LED.
    f.cap.clear();
    f.from_daw({0xB0, 0x30 + 3, 0x01});
    EXPECT_SURFACE(f, msg({0xF0, 0x13, 0x01, 0x00, 3, 0x01, 0xF7}));
}

static void test_lcd_writes_only_touched_cells() {
    Fixture f;
    // Write "Kick" at offset 0: touches cell (0,0) only.
    f.from_daw({0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00,
                'K', 'i', 'c', 'k', 0xF7});
    CHECK(f.cap.surface.size() == 1);
    if (f.cap.surface.size() == 1) {
        const Msg want = msg({0xF0, 0x13, 0x01, 0x40, LCD_STATUS_CELL_BASE + 0,
                              0x7F, 0x00, 'K', 'i', 'c', 'k', ' ', ' ', ' ', 0xF7});
        expect("LCD cell 0", {f.cap.surface[0]}, {want}, __LINE__);
    }

    // A write spanning a cell boundary must repaint both cells, and only those.
    f.cap.clear();
    f.from_daw({0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x05,
                'A', 'B', 'C', 0xF7});               // offsets 5,6 then 7
    CHECK(f.cap.surface.size() == 2);

    // Second row: offset 56 is the bottom row, cell 0 -> channel-name base.
    f.cap.clear();
    f.from_daw({0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 56,
                'V', 'o', 'x', 0xF7});
    CHECK(f.cap.surface.size() == 1);
    if (f.cap.surface.size() == 1)
        CHECK(f.cap.surface[0][4] == LCD_CHANNEL_CELL_BASE + 0);

    // Non-printable bytes become spaces rather than reaching the display.
    f.cap.clear();
    f.from_daw({0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0x01, 0x02, 0xF7});
    CHECK(f.cap.surface.size() == 1);
    if (f.cap.surface.size() == 1) {
        CHECK(f.cap.surface[0][7] == ' ');
        CHECK(f.cap.surface[0][8] == ' ');
    }

    // A SysEx that is not the MCU LCD message is ignored, not misparsed.
    f.cap.clear();
    f.from_daw({0xF0, 0x00, 0x00, 0x67, 0x14, 0x12, 0x00, 'X', 0xF7});
    EXPECT_SURFACE(f);
}

// --- 5. meter ballistics ---------------------------------------------------

static void test_meter_decay() {
    Fixture f;
    // First tick establishes the clock and emits the initial all-zero state.
    c8_mcu_tick(&f.m, 0);
    CHECK(f.cap.surface.size() == C8_MCU_STRIPS);

    // Full scale on strip 0: level 12 of 12.
    f.cap.clear();
    f.from_daw({0xD0, (0 << 4) | 12});
    c8_mcu_tick(&f.m, 10);
    CHECK(!f.cap.surface.empty());
    if (!f.cap.surface.empty()) {
        // note = 64 | bits, vel = strip. All six rows lit.
        CHECK(f.cap.surface[0][1] == (64 | 0x3F));
        CHECK(f.cap.surface[0][2] == 0);
    }

    // Nothing further from the host: the meter must fall on its own, or the
    // display freezes at the last value when playback stops.
    f.cap.clear();
    c8_mcu_tick(&f.m, 10 + 600);      // half the decay time
    CHECK(!f.cap.surface.empty());
    if (!f.cap.surface.empty()) {
        const int bits = f.cap.surface[0][1] & 0x3F;
        CHECK(bits != 0x3F);          // fallen
        CHECK(bits != 0x00);          // but not yet silent
    }

    // Past the full decay it reaches zero and stays there.
    f.cap.clear();
    c8_mcu_tick(&f.m, 10 + 2000);
    CHECK(!f.cap.surface.empty());
    if (!f.cap.surface.empty()) CHECK((f.cap.surface[0][1] & 0x3F) == 0);

    // Unchanged rows are not resent -- the surface link is the scarce resource.
    f.cap.clear();
    c8_mcu_tick(&f.m, 10 + 3000);
    EXPECT_SURFACE(f);
}

static void test_meter_peak_holds_then_falls() {
    Fixture f;
    c8_mcu_tick(&f.m, 0);
    f.cap.clear();

    // A peak must not be lowered by a smaller value arriving immediately after.
    f.from_daw({0xD0, (3 << 4) | 12});
    f.from_daw({0xD0, (3 << 4) | 2});
    c8_mcu_tick(&f.m, 5);

    bool saw_strip3 = false;
    for (const auto& s : f.cap.surface)
        if (s[2] == 3) {
            saw_strip3 = true;
            CHECK((s[1] & 0x3F) == 0x3F);   // still full scale
        }
    CHECK(saw_strip3);
}

static void test_meter_decay_disabled_follows_host() {
    Fixture f;
    f.m.meter_decay_ms = 0;
    c8_mcu_tick(&f.m, 0);
    f.cap.clear();

    f.from_daw({0xD0, (1 << 4) | 12});
    c8_mcu_tick(&f.m, 10);
    f.cap.clear();

    // With ballistics off the level must hold exactly, not fall.
    c8_mcu_tick(&f.m, 5000);
    EXPECT_SURFACE(f);
}

// --- 6. robustness ---------------------------------------------------------

static void test_truncated_and_junk_input() {
    Fixture f;
    // Nothing below should read past the end or emit anything.
    const uint8_t empty[1] = {0};
    c8_mcu_from_surface(&f.m, empty, 0);
    c8_mcu_from_daw(&f.m, empty, 0);
    c8_mcu_from_surface(&f.m, nullptr, 3);
    c8_mcu_from_daw(&f.m, nullptr, 3);

    f.from_surface({0x90, 0x01});             // 2 bytes, needs 3
    f.from_daw({0xE0, 0x00});                 // truncated pitchbend
    f.from_daw({0xB0, 0x30});                 // truncated CC
    f.from_daw({0xF0, 0x00, 0xF7});           // SysEx too short for a header
    f.from_surface({0xF8});                   // realtime clock
    f.from_daw({0xFE});                       // active sensing

    EXPECT_DAW(f);
    EXPECT_SURFACE(f);
}

static void test_null_callbacks_are_safe() {
    // The dongle wires only one direction during bring-up; a missing callback
    // must discard rather than crash.
    c8_mcu_t m{};
    c8_mcu_init(&m, nullptr, nullptr, nullptr);
    c8_mcu_start(&m);
    const uint8_t note[3] = {0x90, 0x00, 0x40};
    c8_mcu_from_surface(&m, note, sizeof(note));
    const uint8_t pb[3] = {0xE0, 0x00, 0x40};
    c8_mcu_from_daw(&m, pb, sizeof(pb));
    c8_mcu_tick(&m, 100);
}

int main() {
    test_constants_agree();
    test_encoders_match_protocol_cpp();
    test_heartbeat_is_swallowed();
    test_strip_buttons();
    test_fader_touch_not_forwarded();
    test_fader_scaling();
    test_fader_rounding_matches_double();
    test_encoders();
    test_transport_buttons();
    test_nav_mode_retargets_arrows();
    test_recsel_arms_selected_tracks();
    test_led_feedback();
    test_ring_modes();
    test_lcd_writes_only_touched_cells();
    test_meter_decay();
    test_meter_peak_holds_then_falls();
    test_meter_decay_disabled_follows_host();
    test_truncated_and_junk_input();
    test_null_callbacks_are_safe();

    if (g_fail == 0) std::printf("test_mcu: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
