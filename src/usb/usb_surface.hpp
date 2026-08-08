// SPDX-License-Identifier: GPL-3.0-or-later
// Surface implementation that talks to the Command|8's bulk endpoints directly
// with libusb, bypassing every operating system's USB-MIDI class parser.
//
// The device's class-specific bulk-IN endpoint descriptor declares
// bNumEmbMIDIJack 3 with bLength 6 (the required length is 4 + 3 = 7). Linux's
// snd-usb-audio therefore builds no input port, and the macOS and Windows class
// drivers reject the MIDIStreaming interface outright. The endpoints themselves
// are perfectly serviceable -- so this backend opens them from constants and
// never reads a descriptor, which makes the malformation irrelevant rather than
// merely tolerated.
//
// Consequences: no kernel quirk on Linux, no Digidesign driver on Windows (bind
// WinUSB instead), and one code path on all three platforms.
#pragma once

#include <libusb.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "surface.hpp"

namespace command8 {

// Everything the backend needs to know about the device, stated rather than
// discovered. These match the values encoded in the snd-usb-audio quirk.
inline constexpr uint16_t C8_USB_VID = 0x0dba;
inline constexpr uint16_t C8_USB_PID = 0x8000;
inline constexpr int      C8_USB_INTERFACE = 1;     // MIDIStreaming
inline constexpr uint8_t  C8_USB_EP_OUT = 0x01;     // host -> surface
inline constexpr uint8_t  C8_USB_EP_IN  = 0x81;     // surface -> host
inline constexpr int      C8_USB_EP_SIZE = 64;      // Full-Speed bulk

// The device carries three cables: 0 is the control surface, 1 and 2 are the
// rear DIN jacks. Only cable 0 is a Surface; the DIN jacks are MIDI ports and
// belong behind MidiPort if they are ever wired up.
inline constexpr uint8_t C8_CABLE_SURFACE = 0;

class UsbSurface : public Surface {
public:
    ~UsbSurface() override;

    // port_match is accepted for interface compatibility and ignored: this
    // backend matches on VID/PID, which is identical on every platform.
    bool open(const std::string& port_match = kDefaultPortMatch) override;
    void close() override;
    void send(const std::vector<uint8_t>& bytes) override;
    void run() override;
    void stop() override { running_ = false; }
    bool device_present() override { return present_.load(); }

private:
    void keepalive_loop();
    void dispatch_packet(const uint8_t* p);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* dev_ = nullptr;
    bool claimed_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> present_{false};
    std::mutex out_mutex_;
    std::thread keepalive_thread_;
};

// Convert a raw MIDI byte stream into USB-MIDI 1.0 event packets on `cable`.
// Exposed for testing; see tests/test_usb_packets.cpp.
void usb_midi_packetize(const std::vector<uint8_t>& midi, uint8_t cable,
                        std::vector<uint8_t>& out);

}  // namespace command8
