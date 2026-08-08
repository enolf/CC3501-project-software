#include "peripherals/switches/switches.h"

#include "pico/stdlib.h"

#include "timings.h"
#include "board.h"
#include "drivers/digital_switch/digital_switch.h"
#include "drivers/logging/logging.h"
#include "peripherals/events/events.h"

namespace switches {

namespace {

// Two instances of the same driver — which is exactly why DigitalSwitch is a
// class rather than a set of file-scope statics (CLAUDE.md section 4).
//
// Both switches are Normally Open and wired to 3V3 through the switch, with the
// 100 kOhm/100 nF network to ground defining the idle level, so both are
// active-high and neither needs an internal pull. All four of those facts come
// from board.h; nothing about the wiring is repeated here.
DigitalSwitch door(LIMIT_SWITCH_PIN, LIMIT_SWITCH_ACTIVE_HIGH);
DigitalSwitch user_button(USER_SWITCH_PIN, USER_SWITCH_ACTIVE_HIGH);


uint32_t pressed_at_ms = 0;
bool hold_fired = false;

} // namespace

// ---------------------------------------------------------------------------
// READ THIS BEFORE CHANGING ANYTHING BELOW
// ---------------------------------------------------------------------------
// The door mapping is inverted relative to how it reads at a glance, and
// getting it backwards would be a genuinely nasty bug: the system would try to
// start a transaction as the customer opens the fridge and abandon it as they
// close it — the exact opposite of the intended flow, and it would look
// plausible while doing it.
//
//   The limit switch is held CLOSED by the shut door.
//   Switch closed  -> GPIO reads HIGH -> DigitalSwitch reports "pressed".
//   So  "pressed"  ==  door CLOSED,  and  "released"  ==  door OPEN.
//
// Hence wasPressed() raises DoorClosed and wasReleased() raises DoorOpened.
// That is not a mistake.
//
// The polarity is also deliberately fail-safe (dashboard.md section 4.1): a cut
// or disconnected wire lets the 100 kOhm pull the pin low, which reads as
// "door open" — an obviously wrong state that gets investigated, rather than
// "door closed" forever, which would silently hide the fault.

void init()
{
    door.init();
    user_button.init();

    logf(LogLevel::INFORMATION, "switches: door starts %s",
         door.isPressed() ? "CLOSED" : "OPEN");
}

void run_switches()
{
    // Polled every pass rather than on a timer. DigitalSwitch debounces against
    // the wall clock, not against a poll count, so calling it more often only
    // makes detection prompter — it cannot shorten the debounce window. The
    // cost is two GPIO reads.
    door.update();
    user_button.update();

    // See the block comment above: pressed == closed.
    if (door.wasPressed()) {
        events::push(events::Kind::DoorClosed);
    }
    if (door.wasReleased()) {
        events::push(events::Kind::DoorOpened);
    }

    // The button now has two meanings, so neither can be reported on the press
    // edge:
    //
    //   a short press  ->  UserButtonPressed, raised on RELEASE
    //   a long hold    ->  UserButtonHeld, raised the moment it crosses 3 s,
    //                      while the finger is still down
    //
    // A short press CANNOT be reported on the press edge, because at that
    // instant it is not yet known which of the two this is — every hold begins
    // with a press, so doing it that way would fire both actions for one
    // gesture. Waiting for the release is what tells them apart.
    //
    // The hold fires as soon as it qualifies rather than on release, so the
    // person gets their feedback at three seconds instead of whenever they
    // happen to let go. `hold_fired` then suppresses the release, so a long
    // hold produces exactly one event.
    if (user_button.wasPressed()) {
        pressed_at_ms = to_ms_since_boot(get_absolute_time());
        hold_fired = false;
    }

    if (user_button.isPressed() && !hold_fired &&
        (to_ms_since_boot(get_absolute_time()) - pressed_at_ms) >= timings::USER_BUTTON_HOLD_MS) {
        hold_fired = true;
        events::push(events::Kind::UserButtonHeld);
    }

    if (user_button.wasReleased() && !hold_fired) {
        events::push(events::Kind::UserButtonPressed);
    }
}

bool is_door_closed()
{
    return door.isPressed();
}

bool is_user_button_down()
{
    return user_button.isPressed();
}

} // namespace switches
