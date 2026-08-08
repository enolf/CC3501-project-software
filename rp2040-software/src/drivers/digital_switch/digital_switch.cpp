// Generic debounced switch/button driver. See DigitalSwitch.h for the
// wiring assumptions and the NO/NC design rationale.

#include "drivers/digital_switch/digital_switch.h"
#include "hardware/gpio.h"
#include "pico/time.h"

DigitalSwitch::DigitalSwitch(uint pin, bool activeHigh, Pull pull, uint32_t debounceMs)
    : pin(pin), activeHigh(activeHigh), pull(pull), debounceMs(debounceMs)
{
}

void DigitalSwitch::init()
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    switch (pull) {
        case PULL_UP:
            gpio_pull_up(pin);
            break;
        case PULL_DOWN:
            gpio_pull_down(pin);
            break;
        case PULL_NONE:
        default:
            gpio_disable_pulls(pin);
            break;
    }

    // Seed state from the current reading without generating an edge event.
    bool raw = readRawActive();
    debouncedState = raw;
    candidateState = raw;
    candidateSinceMs = to_ms_since_boot(get_absolute_time());
    pressedEvent = false;
    releasedEvent = false;
}

void DigitalSwitch::update()
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw = readRawActive();

    if (raw != candidateState) {
        // Reading changed -- (re)start the stability window from here.
        candidateState = raw;
        candidateSinceMs = now;
        return;
    }

    if (candidateState != debouncedState && (now - candidateSinceMs) >= debounceMs) {
        debouncedState = candidateState;
        if (debouncedState) {
            pressedEvent = true;
        } else {
            releasedEvent = true;
        }
    }
}

bool DigitalSwitch::isPressed() const
{
    return debouncedState;
}

bool DigitalSwitch::wasPressed()
{
    bool result = pressedEvent;
    pressedEvent = false;
    return result;
}

bool DigitalSwitch::wasReleased()
{
    bool result = releasedEvent;
    releasedEvent = false;
    return result;
}

bool DigitalSwitch::readRawActive() const
{
    bool level = gpio_get(pin);
    return activeHigh ? level : !level;
}
