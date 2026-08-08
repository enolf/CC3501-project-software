#pragma once

// The one and only reader of the debug serial input.
//
// Several modules want to be driven from the keyboard so the system can be
// exercised without the hardware they stand in for — the Pi link simulator, the
// fake coin events, the fake door. If each called getchar_timeout_us() itself, they would
// race for the same character stream and keystrokes would vanish into whichever
// module happened to be polled first. So exactly one module reads stdin, and
// everything else is handed the result.
//
// Keys are dispatched by the caller in an explicit order (see main.cpp) rather
// than through a registration mechanism, because with three consumers the
// explicit version is shorter and far easier to follow.

namespace sim_input {

/// Fetch the next keystroke, if one is waiting.
///
/// Returns true and sets `key` when a character has been typed; returns false
/// immediately otherwise. Never blocks, so it is safe to call every pass.
bool poll(int &key);

/// Hand a key to the debug layer from somewhere other than stdin.
///
/// Used by the serial Pi-link backend. With the real link compiled, that
/// backend owns the byte stream — it must, or protocol frames would arrive with
/// characters missing — so it forwards single-character lines here instead.
/// The practical difference is that a debug key then needs Enter after it.
void inject(int key);

} // namespace sim_input
