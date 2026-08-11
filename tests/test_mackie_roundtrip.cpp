// SPDX-License-Identifier: GPL-3.0-or-later
// MackieBackend delegates all translation to the shared C module in src/mcu/,
// which works in Command|8 wire bytes. But Controller hands back-ends decoded
// Events, so the adapter has to rebuild the bytes the device originally sent.
//
// That round trip must be lossless. If it is not, control values shift by a
// least-significant bit or two -- a fader that never quite reaches unity, an
// encoder that stops responding -- which is the kind of fault that gets blamed
// on hardware. So every possible input is driven through decode_* and back here,
// exhaustively rather than by sampling: the domains are small enough that there
// is no reason to guess.
#include <cstdio>
#include <cstdint>

#include "mackie/mackie_backend.hpp"
#include "protocol.hpp"

using namespace command8;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

// Every fader CC the device can send: 8 strips x 8 low-bit groups x 128 values.
// Decode to the normalized value Controller would pass, rebuild the bytes, and
// require them identical.
static void test_fader_roundtrip_is_exact() {
    int checked = 0;
    for (int strip = 0; strip < 8; ++strip) {
        for (int hi = 0; hi < 8; ++hi) {
            const uint8_t cc = static_cast<uint8_t>((hi << 3) | strip);
            for (int val = 0; val < 128; ++val) {
                const Event ev = decode_cc(cc, static_cast<uint8_t>(val));
                const auto* f = std::get_if<FaderMoveEvent>(&ev);
                if (!f) { CHECK(f != nullptr); return; }

                // Exactly what Controller::dispatch computes.
                const double v01 = f->value10 / 1023.0;

                uint8_t out[3];
                surface_bytes_for_fader(f->fader, v01, out);

                if (out[0] != 0xB0 || out[1] != cc || out[2] != val) {
                    std::printf("FAIL %s:%d  fader strip=%d hi=%d val=%d: "
                                "value10=%u -> %02X %02X %02X, wanted B0 %02X %02X\n",
                                __FILE__, __LINE__, strip, hi, val,
                                f->value10, out[0], out[1], out[2], cc, val);
                    ++g_fail;
                    return;
                }
                ++checked;
            }
        }
    }
    CHECK(checked == 8 * 8 * 128);
}

// The encoder's two detent values must survive, on every strip.
static void test_encoder_roundtrip_is_exact() {
    for (int strip = 0; strip < 8; ++strip) {
        for (const uint8_t detent : {ENC_RIGHT, ENC_LEFT}) {
            const uint8_t cc = static_cast<uint8_t>(ENCODER_CC_BASE + strip);
            const Event ev = decode_cc(cc, detent);
            const auto* e = std::get_if<EncoderEvent>(&ev);
            if (!e) { CHECK(e != nullptr); return; }

            uint8_t out[3];
            surface_bytes_for_encoder(e->encoder, e->delta, out);
            CHECK(out[0] == 0xB0);
            CHECK(out[1] == cc);
            CHECK(out[2] == detent);
        }
    }
}

// Every note/velocity pair the surface can produce as a button. Heartbeats and
// fader-touch never reach a Backend (Controller routes them elsewhere), so they
// are excluded -- but note 0 at other velocities IS Select and must survive.
static void test_button_roundtrip_is_exact() {
    int checked = 0;
    for (int note = 0; note < 128; ++note) {
        for (int vel = 0; vel < 128; ++vel) {
            if (note == HEARTBEAT_NOTE && vel == HEARTBEAT_VEL) continue;

            const Event ev = decode_note_on(static_cast<uint8_t>(note),
                                            static_cast<uint8_t>(vel));

            uint8_t out[3];
            if (const auto* b = std::get_if<ButtonEvent>(&ev)) {
                // Select/mute/solo go through on_select/on_mute/on_solo, which
                // forward to on_button with the same (note, subid) -- so one
                // encoder covers every button path.
                surface_bytes_for_button(b->note, b->subid, b->pressed, out);
            } else if (std::get_if<FaderTouchEvent>(&ev)) {
                continue;   // never forwarded to the translator
            } else {
                continue;   // fader-touch or unrecognised
            }

            if (out[0] != 0x90 || out[1] != note || out[2] != vel) {
                std::printf("FAIL %s:%d  button note=%d vel=%d -> "
                            "%02X %02X %02X, wanted 90 %02X %02X\n",
                            __FILE__, __LINE__, note, vel,
                            out[0], out[1], out[2], note, vel);
                ++g_fail;
                return;
            }
            ++checked;
        }
    }
    // Sanity: the loop must actually have exercised a large number of pairs,
    // otherwise a decode change could silently empty this test.
    CHECK(checked > 15000);
}

// The strip-button helpers must agree with the generic one, since the adapter
// implements them by delegation.
static void test_strip_helpers_match_generic() {
    for (int strip = 0; strip < 8; ++strip) {
        for (const bool pressed : {true, false}) {
            for (const uint8_t note : {NOTE_SELECT, NOTE_MUTE, NOTE_SOLO}) {
                uint8_t a[3];
                surface_bytes_for_button(note, static_cast<uint8_t>(strip), pressed, a);
                const Event ev = decode_note_on(a[1], a[2]);
                const auto* b = std::get_if<ButtonEvent>(&ev);
                if (!b) { CHECK(b != nullptr); return; }
                CHECK(b->subid == strip);
                CHECK(b->pressed == pressed);
                CHECK(b->note == note);
            }
        }
    }
}

int main() {
    test_fader_roundtrip_is_exact();
    test_encoder_roundtrip_is_exact();
    test_button_roundtrip_is_exact();
    test_strip_helpers_match_generic();

    if (g_fail == 0) std::printf("test_mackie_roundtrip: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
