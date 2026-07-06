# Command|8 native MIDI protocol — findings

All messages are on **MIDI channel 1**. Status legend: ✅ confirmed live on this
hardware (findings 2026-06-28 – 2026-07-01), 📄 from the neyrinck writeup only,
❓ tentative / needs a targeted test. Raw capture logs are in [captures/](captures/);
some LED/ring/LCD details were decoded from Pro Tools USB captures (Cynthion).

## Inputs (surface → host) — all ✅ verified live

| Control      | Message    | Decode                                                                 |
|--------------|------------|-----------------------------------------------------------------------|
| Fader move   | CC 0–63    | fader = `cc & 0x07`; 7-bit pos = `value`; 10-bit pos = `(value<<3)\|((cc>>3)&7)` |
| Fader touch  | Note 5     | fader = `vel & 0x07`; touched = `vel & 0x40`                            |
| Encoder turn | CC 64–71   | encoder = `cc-64`; `0x41` = right (+1), `0x3F` = left (−1)              |
| Button       | Note-On    | pressed = `vel & 0x40`; **identity = (note, subid)**, subid = `vel & 0x3F` |

### Button identity is the (note, subid) pair

A button is **not** identified by its note alone — and neither is a fader touch.
`subid = velocity & 0x3F` selects the scheme:

* **subid 0–7 = strip-indexed controls** — `note` picks the function, `subid` is
  the strip/fader:

  | note | control            | confirmed |
  |------|--------------------|-----------|
  | 0    | select button      | ✅ (press) |
  | 2    | solo button        | ✅         |
  | 3    | mute button        | ✅         |
  | 5    | **fader touch**    | ✅         |

* **subid 10–14 = discrete buttons** — `note` = position within a bank, `subid` =
  the bank (banks **10, 11, 12, 13, 14**; 14 holds transport). The *same note* is
  reused across banks and across the strip controls above — e.g. **note 5 is
  fader-touch at subid 0–7 but a discrete button (ConC etc.) at subid 10–14**, and
  note 2 is Solo (subid 0–7) and a discrete button (subid 13). So decoding must
  always disambiguate on `subid`, never on note alone.

  This was the "dead buttons" bug: the decoder hijacked *all* note 5 as fader
  touch, so the note-5 bank buttons (ConC at `5,10`, etc.) decoded as
  `FaderTouchEvent` and were dropped by the labeler. Fixed 2026-06-29 (see
  `docs/captures/2026-06-29_note5-overload.log`). **All 47 buttons are now
  labelled** in `command8/profiles/command8_buttons.json` (including the note-5
  bank and the 4 modifiers).

## Outputs (host → surface)

**The surface ignores ALL output until brought online** — see Handshake below.

All strip LEDs use the same velocity form: **vel = `0x40 | strip` to light, `strip`
to clear**.

| Target            | Message                                            | Status |
|-------------------|----------------------------------------------------|--------|
| Button / strip LED| Note-On, note = category, vel = `0x40\|subid` on / `subid` off | ✅ |
| Select LED (green)| Note 1, vel = `0x40\|strip` on / `strip` off       | ✅ — input(note 0) ≠ LED(note 1), asymmetry confirmed |
| Record-arm LED    | **Note 4**, vel = `0x40\|strip` on / `strip` off   | ✅ — the per-strip rec-ready LED (earlier mislabelled "red select"; note 4 is NOT red-select) |
| Select-button surface LED | **Note 0**, vel = `0x40\|strip` on / `strip` off | ✅ — the LED in the select button's surface. Pro Tools lights it by echoing the select **press** (note 0) back; the device does **not** echo it, so there's no phantom-press loop |
| Meter LEDs        | Note-On, note = `64 \| row_bitfield`, vel = strip; note 64 clears | ✅ — fill bottom-up (LED nearest the fader = bit toward the high end; see note) |
| Encoder ring LEDs | SysEx `F0 13 01 00 <enc_addr> <leds> F7`            | ✅ — see Encoder rings below |
| Motor fader       | CC, controller = fader index (0-7), value = 7-bit pos | ✅ |
| LCD text          | SysEx `F0 13 01 40 <cell> 7F 00 <7 ASCII> F7`      | ✅ — see LCD below |

### Encoder rings

Wire form `F0 13 01 00 <enc_addr> <leds> F7`:

* `enc_addr` low 3 bits = encoder (0–7); its **high nibble carries LEDs 8–11**:
  `enc_addr = (encoder & 7) | (((leds >> 7) << 3) & 0x78)`.
* `leds` low 7 bits = LEDs 1–7. So the 11-LED ring is addressed by an 11-bit
  `leds` field split across the two bytes.
* **A single set bit = one lit LED** (a position **dot**): centre (LED 6) = `0x20`,
  hard-left (LED 1) = `0x01`, hard-right (LED 11) = bit 10. `(1<<n)-1` = a
  proportional **fill**. Pro Tools uses a single dot for pan.
* **Both-ways feedback:** Pro Tools echoes a ring update on *every* pan change —
  knob or mouse. Reaper does **not** echo device-originated changes, so the driver
  must paint the ring **locally on knob turns** to match (see `reaper.py`
  `_active_ring`). Decoded from `~/Desktop/c8bothwaypan.pcapng`.

### LCD

Two-row display, 8 cells across (one above each strip), 7 chars/cell. Wire form
`F0 13 01 40 <cell> 7F 00 <7 ASCII> F7` (the `7F 00` after `<cell>` are constant;
text is space-padded / truncated to 7):

* `<cell>` **0x10–0x17** = the 8 channel-name cells (**bottom row**).
* `<cell>` **0x00–0x07** = the 8 **top-row** cells; **0x08–0x0F alias the same top
  row** (last write wins).

Meter fill note: the 6 meter LEDs are addressed top-to-bottom, so to fill from the
bottom LED up use `((1<<rows)-1) << (6-rows)`.

## Handshake / heartbeat ✅

* **Wake:** the surface ignores all output (LEDs/faders/meters/rings) until the
  host sends **Note-On note 0 velocity 127**. After that, output works.
* **Heartbeat:** the surface emits the same `note 0 vel 127` on its own (~5-6s),
  and **echoes every host heartbeat exactly once**. Once woken it stays online
  through at least 12s of host silence.
* **Keepalive:** send `note 0 vel 127` on a timer (NOT in reply to the device —
  replying creates an echo loop). `command8/surface.py` wakes on open and pings
  every `KEEPALIVE_INTERVAL` (4s).
* **Decode:** `note 0 vel 127` is filtered as `HeartbeatEvent`, never a control.
  It is unambiguous vs. real input — select-strip presses are `note 0 vel 64-71`.

## Modifiers

The four modifiers live at **bank 10** (subid 10), notes **8/9/10/11** =
`mod_shift` / `mod_option` / `mod_ctrl` / `mod_cmd` (left→right), confirmed live.
Convention (`command8/buttons.py`): a label starting with `mod_` makes that button
a held modifier — `command8/mapping.py` tracks it in a `held` set and passes
`modifiers=frozenset(...)` to `profile.surface_to_daw()` instead of emitting an
action (the Reaper profile uses this for shift = fine adjust / redo).

## Open items

1. (Optional) measure the true offline timeout to tune `KEEPALIVE_INTERVAL`.
2. (Optional) identify the QuickPunch **footswitch** input event (no footswitch
   available to capture yet).
