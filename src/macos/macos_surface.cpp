// SPDX-License-Identifier: GPL-3.0-or-later
#include "macos/macos_surface.hpp"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace command8 {

namespace {

constexpr int kReadTimeoutMs = 20;    // short: run() also drives the tick
constexpr int kWriteTimeoutMs = 1000;
constexpr int kUsbPacketBytes = 4;
constexpr int kReadBufferBytes = 64;  // wMaxPacketSize for both endpoints

// Pack a raw MIDI byte sequence into 4-byte USB-MIDI event packets. The first
// nibble of each packet is the cable number (always 0 here); the second is the
// Code Index Number, which tells the device how many of the following three
// bytes are real.
std::vector<uint8_t> to_usb_midi(const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> out;
    if (msg.empty()) return out;

    if (msg[0] == 0xF0) {                       // SysEx: 3 bytes per packet
        for (size_t i = 0; i < msg.size(); i += 3) {
            const size_t left = msg.size() - i;
            uint8_t cin;
            if (left > 3)      cin = 0x4;       // continues
            else if (left == 1) cin = 0x5;      // ends with 1 byte
            else if (left == 2) cin = 0x6;      // ends with 2
            else                cin = 0x7;      // ends with 3
            out.push_back(cin);
            for (size_t k = 0; k < 3; ++k)
                out.push_back(i + k < msg.size() ? msg[i + k] : 0x00);
        }
        return out;
    }

    // Channel voice: CIN is the status nibble.
    out.push_back(static_cast<uint8_t>(msg[0] >> 4));
    out.push_back(msg[0]);
    out.push_back(msg.size() > 1 ? msg[1] : 0x00);
    out.push_back(msg.size() > 2 ? msg[2] : 0x00);
    return out;
}

bool find_device(libusb_context* ctx) {
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) return false;
    bool found = false;
    for (ssize_t i = 0; i < n && !found; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
            desc.idVendor == kUsbVendorId && desc.idProduct == kUsbProductId)
            found = true;
    }
    libusb_free_device_list(list, 1);
    return found;
}

}  // namespace

MacosSurface::~MacosSurface() { close(); }

bool MacosSurface::open(const std::string& /*port_match*/) {
    if (libusb_init(&ctx_) != 0) {
        std::fprintf(stderr, "command8: cannot initialise libusb\n");
        ctx_ = nullptr;
        return false;
    }

    // Distinguish "not on the bus" from "on the bus but we cannot open it" -
    // they have completely different fixes, and libusb collapses both into a
    // null handle.
    const bool on_bus = find_device(ctx_);
    handle_ = libusb_open_device_with_vid_pid(ctx_, kUsbVendorId, kUsbProductId);
    if (!handle_) {
        if (!on_bus) {
            // Not conclusive: unprivileged libusb on macOS only enumerates
            // devices it is allowed to touch, so a device held by another
            // process - or simply requiring privileges - is invisible rather
            // than merely unopenable.
            std::fprintf(stderr,
                         "command8: no Digidesign Command|8 (%04x:%04x) visible.\n"
                         "  If it IS plugged in and powered, this is usually one of:\n"
                         "    - the binary needs privileges: try running with sudo\n"
                         "    - another Command|8 bridge/driver already holds it\n"
                         "  (macOS hides USB devices from unprivileged processes, so\n"
                         "   'absent' and 'not permitted' look identical here.)\n",
                         kUsbVendorId, kUsbProductId);
        } else {
            std::fprintf(stderr,
                         "command8: Command|8 (%04x:%04x) is on the USB bus but "
                         "cannot be opened. Another process is probably holding "
                         "it - stop any other Command|8 bridge/driver - or the "
                         "binary needs privileges (try sudo).\n",
                         kUsbVendorId, kUsbProductId);
        }
        close();
        return false;
    }

    // Best effort: macOS usually reports this unsupported, in which case the
    // claim below is what actually takes the interface from the class driver.
    libusb_set_auto_detach_kernel_driver(handle_, 1);

    const int rc = libusb_claim_interface(handle_, kUsbInterface);
    if (rc != 0) {
        std::fprintf(stderr,
                     "command8: cannot claim USB interface %d: %s\n",
                     kUsbInterface, libusb_strerror(static_cast<libusb_error>(rc)));
        if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
            std::fprintf(stderr,
                         "command8: CoreMIDI's class driver holds this interface. "
                         "Taking it back needs privileges - try running as root "
                         "(sudo), and close any app using the Command|8.\n");
        }
        close();
        return false;
    }
    claimed_ = true;
    present_ = true;

    std::fprintf(stderr, "command8: surface open (usb %04x:%04x interface %d)\n",
                 kUsbVendorId, kUsbProductId, kUsbInterface);

    // Wake the surface, then keep it online. Until this arrives the device
    // ignores every LED/fader/meter/LCD message we send.
    send(heartbeat());
    running_ = true;
    keepalive_thread_ = std::thread(&MacosSurface::keepalive_loop, this);
    return true;
}

void MacosSurface::close() {
    running_ = false;
    if (keepalive_thread_.joinable()) keepalive_thread_.join();
    if (handle_) {
        if (claimed_) {
            libusb_release_interface(handle_, kUsbInterface);
            claimed_ = false;
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
    present_ = false;
}

void MacosSurface::stop() { running_ = false; }

void MacosSurface::send(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    if (!handle_ || bytes.empty()) return;
    std::vector<uint8_t> packets = to_usb_midi(bytes);
    if (packets.empty()) return;

    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, kUsbEndpointOut, packets.data(),
                                        static_cast<int>(packets.size()),
                                        &transferred, kWriteTimeoutMs);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_NO_DEVICE) present_ = false;
        if (!send_warned_) {
            send_warned_ = true;
            std::fprintf(stderr, "command8: USB send failed: %s\n",
                         libusb_strerror(static_cast<libusb_error>(rc)));
        }
    }
}

void MacosSurface::keepalive_loop() {
    // Timer-driven, never a reply: the device echoes host heartbeats, so
    // replying would create an echo loop. Sleep in slices so stop() is prompt.
    auto next = std::chrono::steady_clock::now() + keepalive_interval;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::steady_clock::now() >= next) {
            send(heartbeat());
            next += keepalive_interval;
        }
    }
}

void MacosSurface::handle_packet(uint8_t status, uint8_t d1, uint8_t d2) {
    Event decoded = std::monostate{};
    switch (status & 0xF0) {
        case 0x90: decoded = decode_note_on(d1, d2); break;
        case 0x80: decoded = decode_note_on(d1, 0); break;   // note-off = release
        case 0xB0: decoded = decode_cc(d1, d2); break;
        default:   return;
    }
    if (std::holds_alternative<HeartbeatEvent>(decoded)) return;   // filter
    if (std::holds_alternative<std::monostate>(decoded)) return;
    if (cb_) cb_(decoded);
}

void MacosSurface::run() {
    if (!handle_) return;
    running_ = true;
    uint8_t buf[kReadBufferBytes];

    while (running_) {
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, kUsbEndpointIn, buf,
                                            sizeof(buf), &transferred,
                                            kReadTimeoutMs);
        if (rc == 0) {
            for (int i = 0; i + kUsbPacketBytes <= transferred; i += kUsbPacketBytes)
                handle_packet(buf[i + 1], buf[i + 2], buf[i + 3]);
        } else if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
            std::fprintf(stderr, "command8: device removed\n");
            present_ = false;
            running_ = false;
            break;
        }
        // LIBUSB_ERROR_TIMEOUT is the idle case: nothing to read, fall through
        // to the tick so meter ballistics and other periodic work still run.

        if (tick_cb_) tick_cb_();
    }
}

bool MacosSurface::device_present() {
    if (!ctx_) return false;
    if (!present_) return false;
    return find_device(ctx_);
}

std::unique_ptr<Surface> make_surface() { return std::make_unique<MacosSurface>(); }

void print_midi_ports() {
    // There are no MIDI ports on this backend - the surface is raw USB. Report
    // whether the device is on the bus instead, which is the equivalent check.
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        std::fprintf(stderr, "command8: cannot initialise libusb\n");
        return;
    }
    std::fprintf(stderr, "command8: macOS backend talks raw USB (no MIDI ports).\n");
    std::fprintf(stderr, "  Command|8 (%04x:%04x): %s\n", kUsbVendorId, kUsbProductId,
                 find_device(ctx) ? "present" : "NOT FOUND");
    libusb_exit(ctx);
}

}  // namespace command8
