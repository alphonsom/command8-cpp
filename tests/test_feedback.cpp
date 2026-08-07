// SPDX-License-Identifier: GPL-3.0-or-later
// Feedback meter behaviour, against a fake Surface that records what was sent.
//
// Covers the two things that made meters look wrong on real hardware:
//   * truncation lost the top LED row, and any level below one row read as
//     silence;
//   * without falloff the display froze on the last value the host sent, so
//     meters never fell to zero when playback stopped.
#include "feedback.hpp"
#include <cstdio>
#include <thread>
#include <vector>
using namespace command8;

struct FakeSurface : Surface {
    std::vector<std::vector<uint8_t>> sent;
    bool open(const std::string&) override { return true; }
    void close() override {}
    void send(const std::vector<uint8_t>& b) override { sent.push_back(b); }
    void run() override {} void stop() override {}
    bool device_present() override { return true; }
};

// meter(): [0x90, note = 64|bits, vel = strip]. tick() refreshes every strip,
// so pick out the LAST message addressed to the strip under test.
static int rows_for(const std::vector<std::vector<uint8_t>>& sent, int strip) {
    int rows = -1;
    for (const auto& m : sent) {
        if (m.size() < 3 || m[2] != strip) continue;
        int bits = m[1] & 0x3F, n = 0;
        while (bits) { n += bits & 1; bits >>= 1; }
        rows = n;
    }
    return rows;
}
static size_t writes_for(const std::vector<std::vector<uint8_t>>& sent, int strip) {
    size_t n = 0;
    for (const auto& m : sent) if (m.size() >= 3 && m[2] == strip) ++n;
    return n;
}

int main() {
    int fails = 0;
    // --- 1. rounding vs truncation: 11/12 of full scale must light 6, not 5
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(0);   // isolate rounding from falloff
      fb.meter(0, 11.0/12.0); fb.tick();
      int r = rows_for(s.sent, 0);
      std::printf("  11/12 scale -> %d rows (old truncating code gave 5)  %s\n",
                  r, r == 6 ? "ok" : "FAIL"); fails += (r != 6); }

    // --- 2. any non-zero signal lights at least one LED
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(0);
      fb.meter(0, 1.0/12.0); fb.tick();
      int r = rows_for(s.sent, 0);
      std::printf("  1/12 scale  -> %d rows (must be >=1, was 0)          %s\n",
                  r, r >= 1 ? "ok" : "FAIL"); fails += (r < 1); }

    // --- 3. silence is still silence
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(0);
      fb.meter(0, 0.0); fb.tick();
      int r = rows_for(s.sent, 0);
      std::printf("  0/12 scale  -> %d rows (must be 0)                   %s\n",
                  r, r == 0 ? "ok" : "FAIL"); fails += (r != 0); }

    // --- 4. THE BUG: full scale then host goes silent -> must fall to zero
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(300);            // short, to keep the test quick
      fb.meter(3, 1.0); fb.tick();
      int first = rows_for(s.sent, 3);
      for (int i = 0; i < 40; ++i) {          // 400 ms of ticks, NO host updates
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          fb.tick();
      }
      int last = rows_for(s.sent, 3);
      std::printf("  playback stops: %d rows -> %d rows after 400ms       %s\n",
                  first, last, (first == 6 && last == 0) ? "ok" : "FAIL");
      fails += !(first == 6 && last == 0); }

    // --- 5. decay disabled = follow the host exactly (no falloff)
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(0);
      fb.meter(3, 1.0); fb.tick();
      size_t n_after_peak = writes_for(s.sent, 3);
      for (int i = 0; i < 20; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          fb.tick();
      }
      bool held = (writes_for(s.sent, 3) == n_after_peak);
      std::printf("  decay=0: holds host value, no extra writes           %s\n",
                  held ? "ok" : "FAIL"); fails += !held; }

    // --- 6. unchanged level must not re-send (no USB flood)
    { FakeSurface s; Feedback fb(s);
      fb.set_meter_decay_ms(0);
      fb.meter(2, 0.5);
      for (int i = 0; i < 50; ++i) fb.tick();
      const size_t w = writes_for(s.sent, 2);
      std::printf("  50 ticks at a steady level -> %zu writes to that strip  %s\n",
                  w, w == 1 ? "ok" : "FAIL");
      fails += (w != 1); }

    std::printf("\n  %s\n", fails ? "FAILURES" : "all ballistics tests passed");
    return fails ? 1 : 0;
}
