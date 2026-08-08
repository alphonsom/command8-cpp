// SPDX-License-Identifier: GPL-3.0-or-later
// macOS Surface implementation (raw USB via libusb).
//
// Unlike the ALSA and RtMidi backends this one does not go through a MIDI API
// at all. The Command|8's MIDIStreaming *input* descriptor is malformed, so
// class MIDI parsers do not expose a usable input endpoint: Linux patches
// around it with a snd-usb-audio quirk, but CoreMIDI has no quirk mechanism,
// so on macOS the device's own ports enumerate and stay inert. The only way
// in is to claim the interface and speak USB-MIDI packets directly.
//
// Consequences of that choice:
//   * The device is matched by VID/PID, not by port name, so the port_match
//     argument to open() is ignored.
//   * Claiming the interface takes it away from CoreMIDI's class driver, which
//     is a privileged operation - the binary generally needs root.
//   * Raw MIDI byte sequences from protocol::* must be packed into 4-byte
//     USB-MIDI event packets on the way out, and unpacked on the way in.
#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "surface.hpp"

struct libusb_context;
struct libusb_device_handle;

namespace command8 {

// Digidesign Command|8, USB Audio Class 1.0 MIDIStreaming interface.
inline constexpr uint16_t kUsbVendorId = 0x0DBA;
inline constexpr uint16_t kUsbProductId = 0x8000;
inline constexpr int kUsbInterface = 1;
inline constexpr uint8_t kUsbEndpointOut = 0x01;
inline constexpr uint8_t kUsbEndpointIn = 0x81;

class MacosSurface : public Surface {
public:
    MacosSurface() = default;
    ~MacosSurface() override;
    MacosSurface(const MacosSurface&) = delete;
    MacosSurface& operator=(const MacosSurface&) = delete;

    // port_match is ignored: the device is found by VID/PID.
    bool open(const std::string& port_match = kDefaultPortMatch) override;
    void close() override;
    void send(const std::vector<uint8_t>& bytes) override;
    void run() override;
    void stop() override;
    bool device_present() override;

private:
    void keepalive_loop();
    void handle_packet(uint8_t status, uint8_t d1, uint8_t d2);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false;

    std::thread keepalive_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> present_{false};
    std::mutex out_mutex_;
    bool send_warned_ = false;      // log the first send failure only
};

}  // namespace command8
