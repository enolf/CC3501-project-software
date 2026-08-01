#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

/// Generic debounced digital switch/button driver. Works with any tactile
/// button or mechanical limit switch wired to a single GPIO, in either
/// Normally-Open (NO) or Normally-Closed (NC) configuration -- the only
/// thing that differs between those is which electrical level counts as
/// "active" (pressed / closed / triggered), which is a single constructor
/// flag rather than separate code paths for NO vs NC.
///
/// Designed for the wiring used on this board for the limit switch and user
/// button: one side of the switch to 3V3, the other side to the GPIO, with
/// an external 100 kOhm resistor and 100 nF capacitor from the GPIO node to
/// ground forming an RC low-pass filter (tau = R*C = 10 ms) for hardware
/// debounce. That network also defines the idle level when the switch is
/// open, so no internal pull is needed for this wiring -- but internal
/// pull-up/pull-down is available as an option for a bare switch wired
/// without such a network.
///
/// Usage:
///   DigitalSwitch doorSensor(LIMIT_SWITCH_PIN, LIMIT_SWITCH_ACTIVE_HIGH);
///   doorSensor.init();
///   ...
///   doorSensor.update();                  // call every poll tick
///   if (doorSensor.wasPressed()) { ... }   // fires once per press
class DigitalSwitch {
public:
    /// How long the raw reading must stay stable before a state change is
    /// accepted. Chosen well above the board's RC filter time constant
    /// (100 kOhm * 100 nF = 10 ms) so software debounce isn't fighting
    /// filter ringing, and well above the intended poll interval so several
    /// polls confirm the new state before it is accepted.
    static constexpr uint32_t DEFAULT_DEBOUNCE_MS = 30;

    /// Optional internal pull configuration, for a switch wired without its
    /// own idle-defining network (unlike this board's switches, which have
    /// one via the external RC filter, so they use PULL_NONE).
    enum Pull {
        PULL_NONE,
        PULL_UP,
        PULL_DOWN,
    };

    /// pin: the GPIO the switch is wired to.
    /// activeHigh: true if the GPIO reads HIGH when the switch is actuated
    ///   (pressed / closed / triggered), false if it reads LOW. This is the
    ///   one thing that differs between NO and NC wiring of the same style
    ///   of circuit -- everything else about the driver is identical.
    /// pull: internal pull configuration; PULL_NONE is correct whenever an
    ///   external network (like this board's RC filter) already defines the
    ///   idle level.
    /// debounceMs: how long a reading must be stable before being accepted.
    explicit DigitalSwitch(uint pin, bool activeHigh, Pull pull = PULL_NONE,
                            uint32_t debounceMs = DEFAULT_DEBOUNCE_MS);

    /// Configure the GPIO and seed the debounced state from the current
    /// reading. No edge event is generated for this initial state -- a
    /// switch that is already actuated at boot (e.g. a door already open)
    /// is not a "press" in the edge-detection sense.
    void init();

    /// Poll the switch. Call this frequently and regularly (e.g. every
    /// 10 ms) from the main loop -- the debounce timing above assumes it is.
    void update();

    /// The current debounced state: true if the switch is active
    /// (pressed / closed / triggered).
    bool isPressed() const;

    /// True if the switch became active since the last time this was
    /// called; reading it clears the flag, so each press is reported
    /// exactly once regardless of how long it is held or how often this is
    /// polled while held.
    bool wasPressed();

    /// True if the switch became inactive since the last time this was
    /// called. Same one-shot behaviour as wasPressed().
    bool wasReleased();

private:
    bool readRawActive() const;

    uint pin;
    bool activeHigh;
    Pull pull;
    uint32_t debounceMs;

    bool debouncedState = false;
    bool candidateState = false;
    uint32_t candidateSinceMs = 0;
    bool pressedEvent = false;
    bool releasedEvent = false;
};
