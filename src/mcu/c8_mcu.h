/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Command|8 <-> Mackie Control translation, as a freestanding C99 module.
 *
 * This is the same logic MackieBackend implements on the host, with every
 * dependency removed: no allocation, no floating point, no threads, no C++
 * runtime, no MIDI backend. Bytes in, bytes out through two callbacks. That is
 * what lets it run inside the dongle firmware on an RP2040 -- where core 1 is
 * bit-banging USB and a malloc or a blocking call in the data path would show
 * up as a dropped bus -- while still being compiled and tested on the desktop,
 * which is the only place debugging it is cheap.
 *
 * Fixed point rather than double is not an optimisation. The Cortex-M0+ has no
 * FPU, but more importantly integer arithmetic is exactly reproducible, so the
 * unit tests pin real values instead of tolerances. Every conversion below is
 * chosen to round identically to the lround() it replaces; where the C++ path
 * divides by a constant the integer form is written as (2*N + d) / (2*d) so the
 * half-way case lands the same way.
 *
 * The module holds all its state in c8_mcu_t. Nothing is static, so a test can
 * run many independent instances, and the caller owns the memory.
 */
#ifndef C8_MCU_H
#define C8_MCU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C8_MCU_STRIPS      8
#define C8_MCU_RING_LEDS   11
#define C8_MCU_METER_ROWS  6
#define C8_MCU_LCD_CELLS   112  /* Mackie LCD is 2 rows x 56 columns */
#define C8_MCU_LCD_WIDTH   7    /* characters per Command|8 LCD cell */

/* Meter falloff: milliseconds for a full-scale meter to reach zero. Hosts send
 * meter levels sparsely (Reaper only when the quantised 0-12 level changes,
 * about 1 Hz) and expect the surface to supply ballistics in between, the way
 * real MCU hardware does. 0 disables it and follows the host exactly. */
#define C8_MCU_DEFAULT_METER_DECAY_MS 1200

/* Longest message the module ever emits: an LCD cell SysEx, 7 header + 7 text
 * + F7 = 15 bytes. Sized with headroom so a caller can use it for a buffer. */
#define C8_MCU_MAX_MSG 16

/* Emitted messages are complete MIDI messages, never split across calls. */
typedef void (*c8_mcu_emit_fn)(void *user, const uint8_t *msg, size_t len);

/* Bank/Nudge/Zoom form a radio group local to the surface -- the DAW has no
 * equivalent, so the mode lives here and retargets the arrow buttons. */
typedef enum {
    C8_NAV_BANK  = 0,
    C8_NAV_NUDGE = 1,
    C8_NAV_ZOOM  = 2,
} c8_nav_mode_t;

typedef struct {
    c8_mcu_emit_fn to_daw;      /* Mackie Control messages towards the DAW    */
    c8_mcu_emit_fn to_surface;  /* Command|8 native messages towards the unit */
    void *user;

    uint8_t nav_mode;           /* c8_nav_mode_t */
    uint8_t selected;           /* bitmask of selected strips, bit N = strip N */
    uint8_t lcd[C8_MCU_LCD_CELLS];

    /* Meter ballistics. Levels are Q8 fixed point in meter rows, so full scale
     * is C8_MCU_METER_ROWS << 8. */
    uint16_t meter_level_q8[C8_MCU_STRIPS];
    uint8_t  meter_bits[C8_MCU_STRIPS];
    uint8_t  meter_sent;        /* bitmask: has meter_bits been emitted yet */
    uint32_t meter_t_ms[C8_MCU_STRIPS];
    uint32_t meter_decay_ms;
    bool     meter_t_valid;     /* false until the first tick supplies a clock */
} c8_mcu_t;

/* Either callback may be NULL, in which case that direction is discarded. */
void c8_mcu_init(c8_mcu_t *m, c8_mcu_emit_fn to_daw, c8_mcu_emit_fn to_surface,
                 void *user);

/* Paint the initial surface state (the default nav-mode LED). Call once the
 * surface is known to be online, not before -- LEDs sent to a sleeping unit
 * are lost. */
void c8_mcu_start(c8_mcu_t *m);

/* One complete MIDI message from the Command|8. Emits via to_daw. */
void c8_mcu_from_surface(c8_mcu_t *m, const uint8_t *msg, size_t len);

/* One complete MIDI message from the DAW's Mackie Control. Emits via
 * to_surface. */
void c8_mcu_from_daw(c8_mcu_t *m, const uint8_t *msg, size_t len);

/* Advance meter falloff and emit any rows that changed. now_ms is a free-running
 * millisecond clock; only differences matter, so wrap is harmless. Call at
 * roughly 10 Hz or better. */
void c8_mcu_tick(c8_mcu_t *m, uint32_t now_ms);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* C8_MCU_H */
