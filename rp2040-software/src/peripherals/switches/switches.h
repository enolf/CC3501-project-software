#pragma once

#include <stdbool.h>

// The board's mechanical switches: the fridge door limit switch and the panel
// user button.
//
// Both are the same kind of device, so both go through the same driver
// (drivers/DigitalSwitch). This module owns the two instances, polls them, and
// turns their edges into events. It is the only place that knows which
// electrical edge means which real-world thing.

namespace switches {

/// Configure both switches and seed their debounced state from the current
/// reading. Call once at startup, before the superloop.
///
/// No events are raised for the initial state: a door that is already open at
/// boot is a starting condition, not an edge. Use is_door_closed() to ask what
/// the world actually looks like rather than inferring it from edges alone.
void init();

/// Poll both switches and raise events for any edge. Call every pass of the
/// superloop; it is cheap and non-blocking (two GPIO reads and a comparison).
void run_switches();

/// Current debounced door state.
///
/// Worth having as well as the events: the state machine needs to know whether
/// the door is shut when deciding whether to advance, and reconstructing that
/// from a history of edges is both harder and easier to get wrong.
bool is_door_closed();

/// Current debounced state of the user button (true while held).
bool is_user_button_down();

} // namespace switches
