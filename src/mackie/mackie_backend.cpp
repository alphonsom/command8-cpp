// SPDX-License-Identifier: GPL-3.0-or-later
#include "mackie/mackie_backend.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include "feedback.hpp"
#include "protocol.hpp"

namespace command8 {

namespace {

// Milliseconds for the translator's meter ballistics. Only differences matter,
// so the epoch is irrelevant and wrapping is harmless.
uint32_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

MackieBackend::MackieBackend(std::string recv_match, std::string send_match)
    : port_(make_midi_port()) {
    c8_mcu_init(&mcu_, &MackieBackend::to_daw, &MackieBackend::to_surface, this);

    if (!port_->open(recv_match, send_match)) port_.reset();
    if (port_) {
        port_->set_rx([this](const std::vector<uint8_t>& m) {
            c8_mcu_from_daw(&mcu_, m.data(), m.size());
        });
    }
}

MackieBackend::~MackieBackend() { stop(); }

void MackieBackend::on_start() {
    c8_mcu_start(&mcu_);       // paints the default nav-mode LED via to_surface
    if (port_) port_->start();
}

void MackieBackend::stop() {
    if (port_) port_->stop();
}

void MackieBackend::tick() { c8_mcu_tick(&mcu_, now_ms()); }

// --- translator output -----------------------------------------------------

void MackieBackend::to_daw(void* user, const uint8_t* msg, size_t len) {
    auto* self = static_cast<MackieBackend*>(user);
    if (self->port_) self->port_->send(std::vector<uint8_t>(msg, msg + len));
}

void MackieBackend::to_surface(void* user, const uint8_t* msg, size_t len) {
    auto* self = static_cast<MackieBackend*>(user);
    // The module emits Command|8 wire format already, so this goes straight
    // out rather than back through Feedback's encoders.
    if (self->fb_) self->fb_->raw(msg, len);
}

// --- surface input ---------------------------------------------------------

void MackieBackend::feed_surface(uint8_t status, uint8_t d1, uint8_t d2) {
    const uint8_t m[3] = {status, d1, d2};
    c8_mcu_from_surface(&mcu_, m, sizeof(m));
}

void MackieBackend::on_fader(int strip, double value01) {
    uint8_t m[3];
    surface_bytes_for_fader(strip, value01, m);
    feed_surface(m[0], m[1], m[2]);
}

void MackieBackend::on_encoder(int strip, int delta) {
    if (delta == 0) return;   // not a detent; the translator would ignore it too
    uint8_t m[3];
    surface_bytes_for_encoder(strip, delta, m);
    feed_surface(m[0], m[1], m[2]);
}

void MackieBackend::on_select(int strip, bool pressed) {
    on_button(NOTE_SELECT, static_cast<uint8_t>(strip), pressed);
}
void MackieBackend::on_mute(int strip, bool pressed) {
    on_button(NOTE_MUTE, static_cast<uint8_t>(strip), pressed);
}
void MackieBackend::on_solo(int strip, bool pressed) {
    on_button(NOTE_SOLO, static_cast<uint8_t>(strip), pressed);
}

void MackieBackend::on_button(uint8_t note, uint8_t subid, bool pressed) {
    uint8_t m[3];
    surface_bytes_for_button(note, subid, pressed, m);
    feed_surface(m[0], m[1], m[2]);
}

}  // namespace command8
