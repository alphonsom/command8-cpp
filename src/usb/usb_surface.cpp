// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb/usb_surface.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// ALSA is Linux-only. Guarding on !_WIN32 would drag <alsa/asoundlib.h> into the
// macOS build, where it does not exist.
#if defined(__linux__)
#include "alsa/alsa_surface.hpp"
#endif

namespace command8 {
namespace {

constexpr uint8_t CIN_SYSEX_START = 0x4;
constexpr uint8_t CIN_SYSEX_END_1 = 0x5;
constexpr uint8_t CIN_SYSEX_END_2 = 0x6;
constexpr uint8_t CIN_SYSEX_END_3 = 0x7;
constexpr uint8_t CIN_NOTE_OFF = 0x8;
constexpr uint8_t CIN_NOTE_ON = 0x9;
constexpr uint8_t CIN_CC = 0xB;
constexpr uint8_t CIN_SINGLE_BYTE = 0xF;

void emit(std::vector<uint8_t>& out, uint8_t header, uint8_t b1, uint8_t b2, uint8_t b3) {
    out.push_back(header);
    out.push_back(b1);
    out.push_back(b2);
    out.push_back(b3);
}

}  // namespace

// USB-MIDI 1.0 §4: every event is 4 bytes, the first holding the cable number in
// the high nibble and a Code Index Number in the low nibble. The CIN encodes how
// many of the following three bytes are real, so the receiver never has to run a
// MIDI parser.
void usb_midi_packetize(const std::vector<uint8_t>& midi, uint8_t cable,
                        std::vector<uint8_t>& out) {
    const uint8_t cn = static_cast<uint8_t>(cable << 4);
    size_t i = 0;

    while (i < midi.size()) {
        const uint8_t st = midi[i];

        // SysEx: split into 3-byte groups, with the CIN of the final group
        // reporting how many bytes it carries.
        if (st == 0xF0) {
            size_t end = i;
            while (end < midi.size() && midi[end] != 0xF7) ++end;
            if (end >= midi.size()) return;  // unterminated: drop rather than guess

            size_t pos = i;
            size_t n = end - i + 1;
            while (n > 3) {
                emit(out, static_cast<uint8_t>(cn | CIN_SYSEX_START), midi[pos], midi[pos + 1],
                     midi[pos + 2]);
                pos += 3;
                n -= 3;
            }
            const uint8_t cin = (n == 1) ? CIN_SYSEX_END_1
                              : (n == 2) ? CIN_SYSEX_END_2
                                         : CIN_SYSEX_END_3;
            emit(out, static_cast<uint8_t>(cn | cin), midi[pos], (n >= 2) ? midi[pos + 1] : 0,
                 (n >= 3) ? midi[pos + 2] : 0);
            i = end + 1;
            continue;
        }

        // Realtime bytes may appear anywhere and are always one byte.
        if (st >= 0xF8) {
            emit(out, static_cast<uint8_t>(cn | CIN_SINGLE_BYTE), st, 0, 0);
            ++i;
            continue;
        }

        if (st >= 0x80) {
            const uint8_t hi = static_cast<uint8_t>(st & 0xF0);
            size_t len;
            uint8_t cin;
            if (st == 0xF1 || st == 0xF3) {
                len = 2;
                cin = 0x2;
            } else if (st == 0xF2) {
                len = 3;
                cin = 0x3;
            } else if (st == 0xF6) {
                len = 1;
                cin = CIN_SYSEX_END_1;  // doubles as single-byte system common
            } else if (hi == 0xC0 || hi == 0xD0) {
                len = 2;
                cin = static_cast<uint8_t>(hi >> 4);
            } else {
                len = 3;
                cin = static_cast<uint8_t>(hi >> 4);
            }
            if (i + len > midi.size()) return;  // truncated message
            emit(out, static_cast<uint8_t>(cn | cin), midi[i], (len >= 2) ? midi[i + 1] : 0,
                 (len >= 3) ? midi[i + 2] : 0);
            i += len;
            continue;
        }

        // A data byte with no status: the encoders never emit running status, so
        // this is malformed input. Skip it rather than desynchronising.
        ++i;
    }
}

UsbSurface::~UsbSurface() { close(); }

bool UsbSurface::device_present() {
    // Once open, run() and send() maintain present_ from transfer results, which
    // notices removal faster than a scan would. Before open there is no handle,
    // so walk the bus instead of reporting a flag that is false by construction.
    if (dev_) return present_.load();

    libusb_context* ctx = ctx_;
    libusb_context* tmp = nullptr;
    if (!ctx) {
        if (libusb_init(&tmp) != LIBUSB_SUCCESS) return false;
        ctx = tmp;
    }

    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < n && !found; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) == LIBUSB_SUCCESS &&
            d.idVendor == C8_USB_VID && d.idProduct == C8_USB_PID)
            found = true;
    }
    if (list) libusb_free_device_list(list, 1);
    if (tmp) libusb_exit(tmp);
    return found;
}

bool UsbSurface::open(const std::string& port_match) {
    (void)port_match;

    if (libusb_init(&ctx_) != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "command8: cannot initialise libusb\n");
        return false;
    }

    dev_ = libusb_open_device_with_vid_pid(ctx_, C8_USB_VID, C8_USB_PID);
    if (!dev_) {
        std::fprintf(stderr,
                     "command8: no Command|8 (%04x:%04x) found, or insufficient "
                     "permission. On Linux add a udev rule granting access; on "
                     "Windows bind WinUSB to the device.\n",
                     C8_USB_VID, C8_USB_PID);
        close();
        return false;
    }

    // Belt and braces. In practice no class driver claims this interface on any
    // platform -- a stock Linux kernel binds neither interface because the
    // descriptors fail to parse -- so there is normally nothing to detach. This
    // covers the case where the quirk-patched snd-usb-audio did bind it, and is
    // a no-op (NOT_SUPPORTED) on macOS and Windows.
    libusb_set_auto_detach_kernel_driver(dev_, 1);

    const int r = libusb_claim_interface(dev_, C8_USB_INTERFACE);
    if (r != LIBUSB_SUCCESS) {
        std::fprintf(stderr,
                     "command8: cannot claim interface %d (%s)%s\n", C8_USB_INTERFACE,
                     libusb_error_name(r),
                     r == LIBUSB_ERROR_BUSY
                         ? " - another process holds it; stop any running command8 daemon"
                         : "");
        close();
        return false;
    }
    claimed_ = true;
    present_ = true;

    std::fprintf(stderr, "command8: surface open (usb %04x:%04x interface %d)\n", C8_USB_VID,
                 C8_USB_PID, C8_USB_INTERFACE);

    // Wake the surface, then keep it online. The device ignores all output until
    // it receives this.
    send(heartbeat());
    running_ = true;
    keepalive_thread_ = std::thread(&UsbSurface::keepalive_loop, this);
    return true;
}

void UsbSurface::close() {
    running_ = false;
    if (keepalive_thread_.joinable()) keepalive_thread_.join();
    if (dev_) {
        if (claimed_) libusb_release_interface(dev_, C8_USB_INTERFACE);
        libusb_close(dev_);
        dev_ = nullptr;
    }
    claimed_ = false;
    present_ = false;
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

void UsbSurface::send(const std::vector<uint8_t>& bytes) {
    if (!dev_ || bytes.empty()) return;

    std::vector<uint8_t> packets;
    packets.reserve(bytes.size() * 2);
    usb_midi_packetize(bytes, C8_CABLE_SURFACE, packets);
    if (packets.empty()) return;

    std::lock_guard<std::mutex> lock(out_mutex_);
    size_t off = 0;
    while (off < packets.size()) {
        const int chunk =
            static_cast<int>(std::min<size_t>(C8_USB_EP_SIZE, packets.size() - off));
        int transferred = 0;
        const int r = libusb_bulk_transfer(dev_, C8_USB_EP_OUT, packets.data() + off, chunk,
                                           &transferred, 100);
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            present_ = false;
            return;
        }
        if (r != LIBUSB_SUCCESS || transferred <= 0) return;
        off += static_cast<size_t>(transferred);
    }
}

void UsbSurface::keepalive_loop() {
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

void UsbSurface::dispatch_packet(const uint8_t* p) {
    if ((p[0] >> 4) != C8_CABLE_SURFACE) return;  // DIN jacks are not surface input

    Event decoded = std::monostate{};
    switch (p[0] & 0x0F) {
        case CIN_NOTE_ON:
            decoded = decode_note_on(p[2], p[3]);
            break;
        case CIN_NOTE_OFF:
            decoded = decode_note_on(p[2], 0);  // vel-0 release
            break;
        case CIN_CC:
            decoded = decode_cc(p[2], p[3]);
            break;
        default:
            return;
    }
    if (std::holds_alternative<HeartbeatEvent>(decoded)) return;  // filtered
    if (std::holds_alternative<std::monostate>(decoded)) return;
    if (cb_) cb_(decoded);
}

void UsbSurface::run() {
    if (!dev_) return;
    running_ = true;

    uint8_t buf[C8_USB_EP_SIZE];
    auto last_tick = std::chrono::steady_clock::now();

    while (running_) {
        int transferred = 0;
        const int r =
            libusb_bulk_transfer(dev_, C8_USB_EP_IN, buf, sizeof(buf), &transferred, 100);

        if (r == LIBUSB_SUCCESS) {
            for (int i = 0; i + 4 <= transferred; i += 4) dispatch_packet(buf + i);
        } else if (r == LIBUSB_ERROR_TIMEOUT) {
            // Idle surface. Not an error: bulk IN NAKs until there is input.
        } else if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            std::fprintf(stderr, "command8: device removed\n");
            present_ = false;
            running_ = false;
            break;
        }

        // The Surface contract promises a ~10 Hz tick; a busy fader would
        // otherwise call it far more often.
        const auto now = std::chrono::steady_clock::now();
        if (tick_cb_ && now - last_tick >= std::chrono::milliseconds(100)) {
            last_tick = now;
            tick_cb_();
        }
    }
}

// ---- factory ----------------------------------------------------------------

std::unique_ptr<Surface> make_surface() {
#if defined(__linux__)
    // Escape hatch: the quirk-patched ALSA path still works where it is
    // installed, and is useful for A/B testing this backend against it.
    const char* backend = std::getenv("COMMAND8_BACKEND");
    if (backend && std::strcmp(backend, "alsa") == 0) {
        std::fprintf(stderr, "command8: using ALSA backend (COMMAND8_BACKEND=alsa)\n");
        return std::make_unique<AlsaSurface>();
    }
#endif
    return std::make_unique<UsbSurface>();
}

void print_midi_ports() {
#if defined(__linux__)
    const char* backend = std::getenv("COMMAND8_BACKEND");
    if (backend && std::strcmp(backend, "alsa") == 0) {
        alsa_print_midi_ports();
        return;
    }
#endif
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "command8: cannot initialise libusb\n");
        return;
    }

    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    std::fprintf(stderr, "USB devices:\n");
    bool found = false;
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != LIBUSB_SUCCESS) continue;
        const bool is_c8 = d.idVendor == C8_USB_VID && d.idProduct == C8_USB_PID;
        if (is_c8) found = true;
        std::fprintf(stderr, "  %04x:%04x  bus %3d  addr %3d%s\n", d.idVendor, d.idProduct,
                     libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]),
                     is_c8 ? "   <- Command|8" : "");
    }
    if (!found) std::fprintf(stderr, "  (no Command|8 found)\n");

    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

}  // namespace command8
