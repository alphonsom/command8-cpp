// SPDX-License-Identifier: GPL-3.0-or-later
// Framework-free unit tests for the USB-MIDI 1.0 event packetiser used by the
// libusb Surface backend.
//
// Every packet is 4 bytes: a header carrying the cable number in the high nibble
// and a Code Index Number in the low nibble, then up to three MIDI bytes. Getting
// the CIN wrong is silent -- the device simply ignores the message -- so the
// encoding is pinned down here rather than discovered on hardware.
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "protocol.hpp"
#include "usb/usb_surface.hpp"

using namespace command8;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static std::vector<uint8_t> pack(std::initializer_list<int> midi, uint8_t cable = 0) {
    std::vector<uint8_t> in, out;
    for (int x : midi) in.push_back(static_cast<uint8_t>(x));
    usb_midi_packetize(in, cable, out);
    return out;
}

static bool eq(const std::vector<uint8_t>& a, std::initializer_list<int> b) {
    if (a.size() != b.size()) return false;
    size_t i = 0;
    for (int x : b)
        if (a[i++] != static_cast<uint8_t>(x)) return false;
    return true;
}

int main() {
    // --- channel voice: 3-byte messages, CIN = status high nibble ---
    CHECK(eq(pack({0x90, 0x00, 0x7F}), {0x09, 0x90, 0x00, 0x7F}));  // note-on
    CHECK(eq(pack({0x80, 0x05, 0x00}), {0x08, 0x80, 0x05, 0x00}));  // note-off
    CHECK(eq(pack({0xB0, 0x40, 0x41}), {0x0B, 0xB0, 0x40, 0x41}));  // CC

    // --- 2-byte channel messages keep their own CIN and length ---
    CHECK(eq(pack({0xC0, 0x03}), {0x0C, 0xC0, 0x03, 0x00}));  // program change
    CHECK(eq(pack({0xD0, 0x40}), {0x0D, 0xD0, 0x40, 0x00}));  // channel pressure

    // --- cable number occupies the header's high nibble ---
    CHECK(eq(pack({0x90, 0x00, 0x7F}, 2), {0x29, 0x90, 0x00, 0x7F}));

    // --- back-to-back messages produce back-to-back packets ---
    CHECK(eq(pack({0x90, 0x00, 0x7F, 0xB0, 0x01, 0x02}),
             {0x09, 0x90, 0x00, 0x7F, 0x0B, 0xB0, 0x01, 0x02}));

    // --- the wake/keepalive heartbeat, which is what actually brings the
    //     surface online, must land as a single note-on packet ---
    {
        std::vector<uint8_t> out;
        usb_midi_packetize(heartbeat(), C8_CABLE_SURFACE, out);
        CHECK(out.size() == 4);
        CHECK(out[0] == 0x09);
        CHECK(out[2] == HEARTBEAT_NOTE);
        CHECK(out[3] == HEARTBEAT_VEL);
    }

    // --- SysEx: exactly 3 bytes ends with CIN 7 ---
    CHECK(eq(pack({0xF0, 0x13, 0xF7}), {0x07, 0xF0, 0x13, 0xF7}));

    // --- SysEx: a 4-byte message splits into a start group then a 1-byte end ---
    CHECK(eq(pack({0xF0, 0x13, 0x01, 0xF7}),
             {0x04, 0xF0, 0x13, 0x01, 0x05, 0xF7, 0x00, 0x00}));

    // --- SysEx: 5 bytes -> start group + 2-byte end ---
    CHECK(eq(pack({0xF0, 0x13, 0x01, 0x00, 0xF7}),
             {0x04, 0xF0, 0x13, 0x01, 0x06, 0x00, 0xF7, 0x00}));

    // --- a real encoder-ring message round-trips to whole packets ---
    {
        const std::vector<uint8_t> ring = encoder_ring(0, 0x20);
        std::vector<uint8_t> out;
        usb_midi_packetize(ring, C8_CABLE_SURFACE, out);
        CHECK(out.size() % 4 == 0);
        CHECK(!out.empty());
        // First packet must be a SysEx start or a complete short SysEx.
        const uint8_t cin = out[0] & 0x0F;
        CHECK(cin == 0x4 || cin == 0x5 || cin == 0x6 || cin == 0x7);
        CHECK(out[1] == 0xF0);
        // Last packet must be a SysEx-end CIN, and 0xF7 must be its final byte.
        const uint8_t last_cin = out[out.size() - 4] & 0x0F;
        CHECK(last_cin == 0x5 || last_cin == 0x6 || last_cin == 0x7);
        const size_t tail = (last_cin == 0x5) ? 3 : (last_cin == 0x6) ? 2 : 1;
        CHECK(out[out.size() - tail] == 0xF7);
    }

    // --- an LCD write is longer than one bulk packet's worth of MIDI and must
    //     still packetise cleanly ---
    {
        const std::vector<uint8_t> text = lcd_channel(0, "CHAN 1");
        std::vector<uint8_t> out;
        usb_midi_packetize(text, C8_CABLE_SURFACE, out);
        CHECK(out.size() % 4 == 0);
        CHECK(out[1] == 0xF0);
    }

    // --- realtime bytes are single-byte packets ---
    CHECK(eq(pack({0xF8}), {0x0F, 0xF8, 0x00, 0x00}));

    // --- malformed input is dropped, never allowed to desynchronise ---
    CHECK(pack({0xF0, 0x13, 0x01}).empty());   // unterminated SysEx
    CHECK(pack({0x90, 0x00}).empty());         // truncated note-on
    CHECK(pack({0x40, 0x41}).empty());         // data bytes with no status

    if (g_fail == 0) std::printf("usb packet tests passed\n");
    return g_fail ? 1 : 0;
}
