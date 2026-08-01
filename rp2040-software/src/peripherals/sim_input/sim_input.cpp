#include "peripherals/sim_input/sim_input.h"

#include "pico/stdlib.h"

namespace sim_input {

namespace {

/// One pending key, injected by the serial backend. A single slot is enough:
/// keys arrive from a human typing, and the superloop drains this thousands of
/// times a second.
int pending = -1;

} // namespace

void inject(int key)
{
    pending = key;
}

bool poll(int &key)
{
    // A key handed over by the Pi-link backend always wins. With the serial
    // backend compiled, this is the ONLY source: that backend owns the byte
    // stream, because a getchar() here would eat protocol bytes and the frames
    // would arrive with holes in them.
    if (pending >= 0) {
        key = pending;
        pending = -1;
        return true;
    }

#if !PI_LINK_SERIAL
    // Simulator build: nothing else is reading stdin, so take characters
    // directly. A timeout of zero means "return immediately if nothing has been
    // typed", so the superloop is never stalled waiting for a human.
    const int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
        key = c;
        return true;
    }
#endif

    return false;
}

} // namespace sim_input
