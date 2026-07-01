// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.hpp"

namespace command8 {

void Controller::dispatch(const Event& ev) {
    if (const auto* b = std::get_if<ButtonEvent>(&ev)) {
        if (b->category == "select")      backend_.on_select(b->subid, b->pressed);
        else if (b->category == "mute")   backend_.on_mute(b->subid, b->pressed);
        else if (b->category == "solo")   backend_.on_solo(b->subid, b->pressed);
        else                              backend_.on_button(b->note, b->subid, b->pressed);
    } else if (const auto* f = std::get_if<FaderMoveEvent>(&ev)) {
        backend_.on_fader(f->fader, f->value10 / 1023.0);   // 10-bit -> 0..1
    } else if (const auto* t = std::get_if<FaderTouchEvent>(&ev)) {
        backend_.on_fader_touch(t->fader, t->touched);
    } else if (const auto* e = std::get_if<EncoderEvent>(&ev)) {
        backend_.on_encoder(e->encoder, e->delta);
    }
}

void Controller::run() {
    surface_.set_callback([this](const Event& ev) { dispatch(ev); });
    surface_.set_tick([this]() { backend_.tick(); });
    backend_.on_start();
    surface_.run();
}

}  // namespace command8
