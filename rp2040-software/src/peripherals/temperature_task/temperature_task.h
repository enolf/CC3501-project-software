#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fridge temperatures, sampled on a timer and reported to the Pi.
//
// The RP2040 does not care what any of these readings mean. It emits raw ROM
// codes and degrees; the Pi maps ROM code to zone name ("freezer", "top
// fridge") on the way into the database. That is deliberate — dashboard.md
// section 4.3: replacing a failed sensor is then editing one row on the Pi
// rather than rebuilding and reflashing firmware. The TFT never shows a
// temperature at all, so zone names have no meaning on this side.
//
// NON-BLOCKING, which the DS18B20 makes awkward: a 12-bit conversion takes
// 750 ms, and the driver's convertAndWait() sleeps through it. That would stall
// the display for three quarters of a second every sample. This task uses the
// driver's separate startConversion() / isConversionDone() calls instead and
// returns immediately, checking back on later passes.

namespace temperature {

/// Discover the sensors on the 1-Wire bus and enable the RP2040's internal
/// temperature sensor. Returns false if no DS18B20 answers, which is survivable
/// — the fridge trades perfectly well without knowing how cold it is.
bool init();

/// Advance the sampling state machine. Call every pass of the superloop.
void run_temperature();

/// How many DS18B20s were found.
uint8_t sensor_count();

/// The RP2040's own die temperature.
///
/// Accurate to only a few degrees in absolute terms, so it is a health and
/// trend signal — "is the enclosure getting hot?" — not a measurement.
float die_celsius();

} // namespace temperature
