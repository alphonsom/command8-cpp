// SPDX-License-Identifier: GPL-3.0-or-later
// Wires a Surface to a Backend: normalizes raw protocol Events into semantic
// back-end calls and provides the Feedback handle.
#pragma once

#include "backend.hpp"
#include "feedback.hpp"
#include "surface.hpp"

namespace command8 {

class Controller {
public:
    Controller(Surface& surface, Backend& backend)
        : surface_(surface), backend_(backend), feedback_(surface) {
        backend_.attach(&feedback_);
    }

    Feedback& feedback() { return feedback_; }

    // Install the dispatch callback and run the (blocking) input loop.
    void run();

private:
    void dispatch(const Event& ev);

    Surface& surface_;
    Backend& backend_;
    Feedback feedback_;
};

}  // namespace command8
