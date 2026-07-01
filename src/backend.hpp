// SPDX-License-Identifier: GPL-3.0-or-later
// Host-integration interface. A back-end receives normalized surface input and
// drives feedback via the Feedback handle. Concrete back-ends (a Reaper OSC
// bridge, a Mackie/HUI translator, a Bitwig backend, …) subclass this; the core
// library ships none — it stays DAW-agnostic.
#pragma once

#include <cstdint>

namespace command8 {

class Feedback;

class Backend {
public:
    virtual ~Backend() = default;

    // Called by the Controller once, before the input loop starts.
    void attach(Feedback* fb) { fb_ = fb; }

    virtual void on_start() {}

    // Periodic callback (~10 Hz) from the input loop; for time-based feedback
    // (e.g. reverting a temporary LCD value). Same thread as the on_* handlers.
    virtual void tick() {}

    // Normalized input. Fader/encoder-driven params are 0..1; encoder deltas ±1.
    virtual void on_fader(int /*strip*/, double /*value01*/) {}
    virtual void on_fader_touch(int /*strip*/, bool /*touched*/) {}
    virtual void on_encoder(int /*strip*/, int /*delta*/) {}
    virtual void on_select(int /*strip*/, bool /*pressed*/) {}
    virtual void on_mute(int /*strip*/, bool /*pressed*/) {}
    virtual void on_solo(int /*strip*/, bool /*pressed*/) {}

    // Discrete / function buttons, keyed on the raw (note, subid) pair. Labeling
    // is a front-end concern (kept out of the DAW-agnostic core).
    virtual void on_button(uint8_t /*note*/, uint8_t /*subid*/, bool /*pressed*/) {}

protected:
    Feedback* fb_ = nullptr;
};

}  // namespace command8
