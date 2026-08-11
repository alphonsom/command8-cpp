/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "c8_mcu.h"

#include <string.h>

/* ---- Command|8 native protocol ------------------------------------------
 * Mirrors protocol.hpp. Kept as literals here rather than shared with the C++
 * header because this module must build with no C++ in sight; test_mcu.cpp
 * includes both and asserts they agree, so a drift is a test failure.
 */
#define C8_HEARTBEAT_NOTE 0
#define C8_HEARTBEAT_VEL  127
#define C8_VEL_ON         0x40  /* velocity bit 6 = pressed / LED-on */
#define C8_SUBID_MASK     0x3F
#define C8_STRIP_MASK     0x07
#define C8_STRIP_SUBID_MAX 7

#define C8_NOTE_SELECT      0
#define C8_NOTE_SOLO        2
#define C8_NOTE_MUTE        3
#define C8_NOTE_FADER_TOUCH 5

#define C8_NOTE_SELECT_GREEN_LED 1
#define C8_NOTE_RECARM_LED       4

#define C8_ENCODER_CC_BASE  64
#define C8_ENCODER_CC_COUNT 8
#define C8_ENC_RIGHT        0x41
#define C8_ENC_LEFT         0x3F
#define C8_FADER_CC_MAX     63

#define C8_METER_NOTE_BASE 64

#define C8_DIGIDESIGN_MFR 0x13
#define C8_RING_DEVICE    0x01
#define C8_RING_CMD       0x00
#define C8_LCD_CMD        0x40
#define C8_LCD_CHANNEL_CELL_BASE 0x10  /* bottom row (channel names) */
#define C8_LCD_STATUS_CELL_BASE  0x00  /* top row */

/* ---- Mackie Control note map --------------------------------------------- */

#define N_REC     0x00
#define N_SOLO    0x08
#define N_MUTE    0x10
#define N_SELECT  0x18

#define N_PLAY    0x5E
#define N_STOP    0x5D
#define N_REC_BTN 0x5F
#define N_REW     0x5B
#define N_FFWD    0x5C
#define N_CYCLE   0x56

#define VPOT_CC     0x10
#define VPOT_LED_CC 0x30

#define N_SEND   0x29
#define N_PAN    0x2A
#define N_PLUGIN 0x2B
#define N_EQ     0x2C
#define N_INST   0x2D

#define N_FLIP   0x32
#define N_BANK_L 0x2E
#define N_BANK_R 0x2F
#define N_CHAN_L 0x30
#define N_CHAN_R 0x31
#define N_CUR_UP 0x60
#define N_CUR_DN 0x61
#define N_CUR_L  0x62
#define N_CUR_R  0x63
#define N_ZOOM   0x64

/* Navigation cluster, all at subid 13. Bank/Nudge/Zoom are a local radio group;
 * ScrlBack/ScrlFwd and ViewUp/Down translate according to the active mode. */
#define NAV_SUBID     13
#define BTN_BANK      2
#define BTN_NUDGE     3
#define BTN_ZOOM      4
#define BTN_SCRL_BACK 5
#define BTN_SCRL_FWD  6
#define BTN_VIEW_UP   7
#define BTN_VIEW_DN   8

/* Command|8 (note, subid) -> MCU note. RecSel and the nav cluster are handled
 * separately in from_surface_note. A flat array beats a std::map here: twelve
 * entries scanned linearly is faster than a tree walk and costs no heap. */
typedef struct { uint8_t note, subid, mcu; } btn_map_t;

static const btn_map_t kBtnToMcu[] = {
    {10, 14, N_PLAY},  { 9, 14, N_STOP},   {11, 14, N_REC_BTN},
    { 3, 14, N_CYCLE}, { 7, 14, N_REW},    { 8, 14, N_FFWD},
    /* V-pot assignment (Pan/Send/Insert/EQ/Dynamics) */
    { 0, 10, N_PAN},   { 1, 10, N_SEND},   { 2, 10, N_PLUGIN},
    { 0, 11, N_EQ},    { 1, 11, N_INST},
    /* Flip: an independent toggle, not part of the nav mode group */
    { 0, 13, N_FLIP},
};

/* MCU note -> Command|8 LED (note, subid). The nav mode LEDs are driven
 * locally, not from DAW feedback, so they are absent here. */
typedef struct { uint8_t mcu, note, subid; } led_map_t;

static const led_map_t kMcuToLed[] = {
    {N_PLAY, 10, 14}, {N_STOP, 9, 14}, {N_REC_BTN, 11, 14}, {N_CYCLE, 3, 14},
    {N_PAN, 0, 10},   {N_SEND, 1, 10}, {N_PLUGIN, 2, 10},   {N_EQ, 0, 11},
    {N_INST, 1, 11},  {N_FLIP, 0, 13},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- helpers ------------------------------------------------------------- */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void emit_daw(c8_mcu_t *m, const uint8_t *b, size_t n) {
    if (m->to_daw) m->to_daw(m->user, b, n);
}

static void emit_surface(c8_mcu_t *m, const uint8_t *b, size_t n) {
    if (m->to_surface) m->to_surface(m->user, b, n);
}

/* --- towards the DAW --- */

static void send_note(c8_mcu_t *m, int note, bool on) {
    const uint8_t b[3] = {0x90, (uint8_t)(note & 0x7F), (uint8_t)(on ? 127 : 0)};
    emit_daw(m, b, sizeof(b));
}

static void send_cc(c8_mcu_t *m, int cc, int value) {
    const uint8_t b[3] = {0xB0, (uint8_t)(cc & 0x7F), (uint8_t)(value & 0x7F)};
    emit_daw(m, b, sizeof(b));
}

static void send_pitch(c8_mcu_t *m, int channel, int value) {
    const int v = clampi(value, -8192, 8191) + 8192;  /* 0..16383 */
    const uint8_t b[3] = {(uint8_t)(0xE0 | (channel & 0x0F)),
                          (uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F)};
    emit_daw(m, b, sizeof(b));
}

/* --- towards the surface --- */

static void surface_button_led(c8_mcu_t *m, uint8_t note, uint8_t subid, bool on) {
    const uint8_t s = (uint8_t)(subid & C8_SUBID_MASK);
    const uint8_t b[3] = {0x90, note, (uint8_t)(on ? (C8_VEL_ON | s) : s)};
    emit_surface(m, b, sizeof(b));
}

static void surface_fader(c8_mcu_t *m, int strip, int value7) {
    const uint8_t b[3] = {0xB0, (uint8_t)(strip & C8_STRIP_MASK),
                          (uint8_t)(value7 & 0x7F)};
    emit_surface(m, b, sizeof(b));
}

static void surface_meter(c8_mcu_t *m, int strip, int level_bits) {
    const uint8_t b[3] = {0x90,
                          (uint8_t)(C8_METER_NOTE_BASE | (level_bits & 0x3F)),
                          (uint8_t)(strip & C8_STRIP_MASK)};
    emit_surface(m, b, sizeof(b));
}

/* Ring LED bitfield: low 7 bits drive LEDs 1-7 in the data byte, bits 7-10 are
 * packed into the high nibble of the address byte. */
static void surface_ring(c8_mcu_t *m, int encoder, uint16_t led_bits) {
    const uint8_t addr = (uint8_t)((encoder & C8_STRIP_MASK) |
                                   (((led_bits >> 7) << 3) & 0x78));
    const uint8_t b[7] = {0xF0, C8_DIGIDESIGN_MFR, C8_RING_DEVICE, C8_RING_CMD,
                          addr, (uint8_t)(led_bits & 0x7F), 0xF7};
    emit_surface(m, b, sizeof(b));
}

static void surface_lcd(c8_mcu_t *m, uint8_t cell, const uint8_t *text) {
    uint8_t b[7 + C8_MCU_LCD_WIDTH + 1];
    b[0] = 0xF0;
    b[1] = C8_DIGIDESIGN_MFR;
    b[2] = C8_RING_DEVICE;
    b[3] = C8_LCD_CMD;
    b[4] = (uint8_t)(cell & 0x7F);
    b[5] = 0x7F;
    b[6] = 0x00;
    for (int i = 0; i < C8_MCU_LCD_WIDTH; i++) {
        const uint8_t c = text[i];
        b[7 + i] = (c >= 0x20 && c <= 0x7E) ? c : 0x20;
    }
    b[7 + C8_MCU_LCD_WIDTH] = 0xF7;
    emit_surface(m, b, sizeof(b));
}

/* ---- meters --------------------------------------------------------------
 * Q8 rows in, Command|8 bitfield out. Rounds rather than truncates: truncation
 * lost the top row (11/12 of full scale lit 5 of 6 LEDs). Any non-zero signal
 * lights at least one LED, so quiet material is distinct from silence, and the
 * fill runs from the high bits down because the meter is addressed top-down.
 */
static int rows_q8_to_bits(uint32_t rows_q8) {
    /* The C++ path tests rows < 0.05, which in Q8 is rows_q8 < 12.8; integer
     * levels make that rows_q8 <= 12. */
    if (rows_q8 < 13) return 0;
    const int n = clampi((int)((rows_q8 + 128) >> 8), 1, C8_MCU_METER_ROWS);
    return ((1 << n) - 1) << (C8_MCU_METER_ROWS - n);
}

static uint32_t meter_decayed(const c8_mcu_t *m, int strip, uint32_t now_ms) {
    uint32_t cur = m->meter_level_q8[strip];
    if (m->meter_decay_ms == 0 || cur == 0 || !m->meter_t_valid) return cur;

    const uint32_t dt_ms = now_ms - m->meter_t_ms[strip];  /* wrap-safe */
    /* Full scale (METER_ROWS rows) falls to zero in meter_decay_ms. */
    const uint32_t drop =
        (dt_ms * (C8_MCU_METER_ROWS << 8)) / m->meter_decay_ms;
    return drop >= cur ? 0 : cur - drop;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void paint_nav_leds(c8_mcu_t *m) {
    surface_button_led(m, BTN_BANK,  NAV_SUBID, m->nav_mode == C8_NAV_BANK);
    surface_button_led(m, BTN_NUDGE, NAV_SUBID, m->nav_mode == C8_NAV_NUDGE);
    surface_button_led(m, BTN_ZOOM,  NAV_SUBID, m->nav_mode == C8_NAV_ZOOM);
}

void c8_mcu_init(c8_mcu_t *m, c8_mcu_emit_fn to_daw, c8_mcu_emit_fn to_surface,
                 void *user) {
    memset(m, 0, sizeof(*m));
    m->to_daw = to_daw;
    m->to_surface = to_surface;
    m->user = user;
    m->nav_mode = C8_NAV_BANK;
    m->meter_decay_ms = C8_MCU_DEFAULT_METER_DECAY_MS;
    memset(m->lcd, ' ', sizeof(m->lcd));
}

void c8_mcu_start(c8_mcu_t *m) {
    memset(m->lcd, ' ', sizeof(m->lcd));
    m->nav_mode = C8_NAV_BANK;
    paint_nav_leds(m);
}

/* ---- Command|8 -> MCU ----------------------------------------------------- */

static void set_nav_mode(c8_mcu_t *m, int mode) {
    if (mode == m->nav_mode) return;
    const bool was_zoom = (m->nav_mode == C8_NAV_ZOOM);
    const bool now_zoom = (mode == C8_NAV_ZOOM);
    m->nav_mode = (uint8_t)mode;
    /* Entering or leaving Zoom toggles the DAW's Zoom modifier so the arrows
     * zoom rather than move the cursor. */
    if (now_zoom != was_zoom) {
        send_note(m, N_ZOOM, true);
        send_note(m, N_ZOOM, false);
    }
    paint_nav_leds(m);
}

static void on_button(c8_mcu_t *m, uint8_t note, uint8_t subid, bool pressed) {
    if (note == 3 && subid == 12) {   /* RecSel: arm the selected track(s) */
        for (int ch = 0; ch < C8_MCU_STRIPS; ch++)
            if (m->selected & (1u << ch)) send_note(m, N_REC + ch, pressed);
        return;
    }

    if (subid == NAV_SUBID) {
        switch (note) {
            case BTN_BANK:  if (pressed) set_nav_mode(m, C8_NAV_BANK);  return;
            case BTN_NUDGE: if (pressed) set_nav_mode(m, C8_NAV_NUDGE); return;
            case BTN_ZOOM:  if (pressed) set_nav_mode(m, C8_NAV_ZOOM);  return;
            case BTN_SCRL_BACK:   /* '<': per-mode step left */
                send_note(m, m->nav_mode == C8_NAV_BANK  ? N_BANK_L
                           : m->nav_mode == C8_NAV_NUDGE ? N_CHAN_L
                                                         : N_CUR_L, pressed);
                return;
            case BTN_SCRL_FWD:    /* '>': per-mode step right */
                send_note(m, m->nav_mode == C8_NAV_BANK  ? N_BANK_R
                           : m->nav_mode == C8_NAV_NUDGE ? N_CHAN_R
                                                         : N_CUR_R, pressed);
                return;
            case BTN_VIEW_UP: send_note(m, N_CUR_UP, pressed); return;
            case BTN_VIEW_DN: send_note(m, N_CUR_DN, pressed); return;
            default: break;   /* Flip (note 0), MstrFadrs (note 1) fall through */
        }
    }

    for (size_t i = 0; i < ARRAY_LEN(kBtnToMcu); i++) {
        if (kBtnToMcu[i].note == note && kBtnToMcu[i].subid == subid) {
            send_note(m, kBtnToMcu[i].mcu, pressed);
            return;
        }
    }
}

static void on_surface_note(c8_mcu_t *m, uint8_t note, uint8_t vel) {
    /* Heartbeat echo. Checked before anything else, exactly as decode_note_on
     * does, because note 0 doubles as Select and velocity 127 would otherwise
     * decode as a Select press on a nonexistent strip 63. Never reply: the
     * device echoes what it receives, so a reply builds a loop. */
    if (note == C8_HEARTBEAT_NOTE && vel == C8_HEARTBEAT_VEL) return;

    const uint8_t subid = (uint8_t)(vel & C8_SUBID_MASK);
    const bool pressed = (vel & C8_VEL_ON) != 0;

    if (subid <= C8_STRIP_SUBID_MAX) {
        switch (note) {
            case C8_NOTE_FADER_TOUCH:
                return;   /* the MCU backend does not forward fader touch */
            case C8_NOTE_SELECT: send_note(m, N_SELECT + (subid & 7), pressed); return;
            case C8_NOTE_SOLO:   send_note(m, N_SOLO   + (subid & 7), pressed); return;
            case C8_NOTE_MUTE:   send_note(m, N_MUTE   + (subid & 7), pressed); return;
            default: break;
        }
    }
    on_button(m, note, subid, pressed);
}

static void on_surface_cc(c8_mcu_t *m, uint8_t cc, uint8_t val) {
    if (cc >= C8_ENCODER_CC_BASE &&
        cc < C8_ENCODER_CC_BASE + C8_ENCODER_CC_COUNT) {
        const int delta = (val == C8_ENC_RIGHT) ? 1 : (val == C8_ENC_LEFT) ? -1 : 0;
        if (delta == 0) return;   /* not a detent: ignore rather than guess */
        const int enc = cc - C8_ENCODER_CC_BASE;
        /* MCU relative-encoder convention: bit 6 set means anticlockwise. */
        send_cc(m, VPOT_CC + (enc & 7), delta > 0 ? 1 : (0x40 | 1));
        return;
    }

    if (cc <= C8_FADER_CC_MAX) {
        const int fader = cc & C8_STRIP_MASK;
        const int hi = (cc >> 3) & C8_STRIP_MASK;   /* 3 extra low-order bits */
        const int value10 = ((int)val << 3) | hi;   /* 0..1023 */
        /* value10/1023 scaled to 0..16383, then biased to MCU's signed range.
         * Written as (2N + d)/(2d) so it rounds like lround(). */
        const int pitch14 = (int)(((int32_t)value10 * 16383 * 2 + 1023) / 2046);
        send_pitch(m, fader & 7, pitch14 - 8192);
    }
}

void c8_mcu_from_surface(c8_mcu_t *m, const uint8_t *msg, size_t len) {
    if (len < 3 || !msg) return;
    switch (msg[0] & 0xF0) {
        case 0x90: on_surface_note(m, msg[1], msg[2]); break;
        case 0xB0: on_surface_cc(m, msg[1], msg[2]); break;
        default: break;
    }
}

/* ---- MCU -> Command|8 ----------------------------------------------------- */

static void lcd_sysex(c8_mcu_t *m, const uint8_t *d, size_t len) {
    /* F0 00 00 66 <model> 12 <offset> <ascii...> F7 */
    if (len < 8 || d[1] != 0x00 || d[2] != 0x00 || d[3] != 0x66 || d[5] != 0x12)
        return;

    const int offset = d[6] & 0x7F;
    const int n = (int)len - 8;   /* strip F0, 6 header bytes and trailing F7 */

    /* Which 7-character cells the write touched. 2 rows x 8 cells fits a
     * 16-bit mask, which replaces the std::set the host version allocates. */
    uint16_t dirty = 0;
    for (int i = 0; i < n; i++) {
        const int p = offset + i;
        if (p < 0 || p >= C8_MCU_LCD_CELLS) continue;
        const uint8_t c = d[7 + i];
        m->lcd[p] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
        const int line = p / 56, col = (p % 56) / 7;
        if (col < 8) dirty |= (uint16_t)(1u << (line * 8 + col));
    }

    for (int line = 0; line < 2; line++) {
        for (int col = 0; col < 8; col++) {
            if (!(dirty & (1u << (line * 8 + col)))) continue;
            const uint8_t *cell = &m->lcd[line * 56 + col * 7];
            surface_lcd(m, (uint8_t)((line == 0 ? C8_LCD_STATUS_CELL_BASE
                                                : C8_LCD_CHANNEL_CELL_BASE) +
                                     (col & C8_STRIP_MASK)),
                        cell);
        }
    }
}

void c8_mcu_from_daw(c8_mcu_t *m, const uint8_t *msg, size_t len) {
    if (!msg || len == 0) return;

    if (msg[0] == 0xF0) {
        lcd_sysex(m, msg, len);
        return;
    }

    const int type = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    switch (type) {
        case 0xE0: {   /* pitchbend = motor fader position */
            if (len < 3) break;
            const int v = ((int)msg[2] << 7) | msg[1];   /* 0..16383 */
            if (ch < C8_MCU_STRIPS) {
                /* v/16383 scaled to 0..127, rounding like lround(). */
                const int val = clampi((int)(((int32_t)v * 127 * 2 + 16383) / 32766),
                                       0, 127);
                surface_fader(m, ch, val);
            }
            break;
        }
        case 0xD0: {   /* channel pressure = meter level */
            if (len < 2) break;
            const int strip = (msg[1] >> 4) & 7;
            const int level = msg[1] & 0x0F;   /* 0..12 in practice */
            /* min(1.0, level/12) of METER_ROWS rows, in Q8: level * 128. */
            uint32_t rows_q8 = (uint32_t)level * 128u;
            const uint32_t full = (uint32_t)C8_MCU_METER_ROWS << 8;
            if (rows_q8 > full) rows_q8 = full;
            /* The host value is a PEAK: rise to it at once and let tick() decay
             * it. Unlike the host version this does not first decay to "now" --
             * from_daw has no clock, and the next tick applies whatever falloff
             * was missed, which at 10 Hz is at most 100 ms of it. With decay
             * disabled the level simply follows the host. */
            const uint32_t cur = m->meter_decay_ms ? m->meter_level_q8[strip] : 0;
            m->meter_level_q8[strip] = (uint16_t)(rows_q8 > cur ? rows_q8 : cur);
            break;
        }
        case 0xB0: {   /* V-pot ring LEDs */
            if (len < 3) break;
            const int cc = msg[1];
            if (cc < VPOT_LED_CC || cc > VPOT_LED_CC + 7) break;
            const int enc = cc - VPOT_LED_CC;
            const int pos = msg[2] & 0x0F, mode = (msg[2] >> 4) & 0x03;
            if (pos == 0) {
                surface_ring(m, enc, 0);
            } else if (mode == 2) {
                /* wrap/fill: pos of 11 LEDs, lit as a thermometer */
                const int p = clampi(pos, 0, C8_MCU_RING_LEDS);
                surface_ring(m, enc, (uint16_t)((1u << p) - 1u));
            } else {
                /* single dot (pan position): pos 1..11 -> LED 0..10 */
                const int p = clampi(pos - 1, 0, C8_MCU_RING_LEDS - 1);
                surface_ring(m, enc, (uint16_t)(1u << p));
            }
            break;
        }
        case 0x90:
        case 0x80: {   /* button LEDs */
            if (len < 3) break;
            const bool on = (type == 0x90 && msg[2] > 0);
            const int n = msg[1];
            if (n >= N_REC && n < N_REC + 8) {
                surface_button_led(m, C8_NOTE_RECARM_LED, (uint8_t)(n - N_REC), on);
            } else if (n >= N_SOLO && n < N_SOLO + 8) {
                surface_button_led(m, C8_NOTE_SOLO, (uint8_t)(n - N_SOLO), on);
            } else if (n >= N_MUTE && n < N_MUTE + 8) {
                surface_button_led(m, C8_NOTE_MUTE, (uint8_t)(n - N_MUTE), on);
            } else if (n >= N_SELECT && n < N_SELECT + 8) {
                const int c = n - N_SELECT;
                if (on) m->selected |= (uint8_t)(1u << c);
                else    m->selected &= (uint8_t)~(1u << c);
                surface_button_led(m, C8_NOTE_SELECT_GREEN_LED, (uint8_t)c, on);
            } else {
                for (size_t i = 0; i < ARRAY_LEN(kMcuToLed); i++) {
                    if (kMcuToLed[i].mcu == n) {
                        surface_button_led(m, kMcuToLed[i].note,
                                           kMcuToLed[i].subid, on);
                        break;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

/* ---- meter ballistics ----------------------------------------------------- */

void c8_mcu_tick(c8_mcu_t *m, uint32_t now_ms) {
    for (int i = 0; i < C8_MCU_STRIPS; i++) {
        const uint32_t cur = meter_decayed(m, i, now_ms);
        m->meter_level_q8[i] = (uint16_t)cur;
        m->meter_t_ms[i] = now_ms;

        const int bits = rows_q8_to_bits(cur);
        const bool sent = (m->meter_sent & (1u << i)) != 0;
        if (sent && bits == m->meter_bits[i]) continue;   /* unchanged */
        m->meter_bits[i] = (uint8_t)bits;
        m->meter_sent |= (uint8_t)(1u << i);
        surface_meter(m, i, bits);
    }
    m->meter_t_valid = true;
}
