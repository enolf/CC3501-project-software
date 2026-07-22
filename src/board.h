#pragma once

// board.h — ALL hardware connectivity lives here.
//
// This project targets a custom PCB, and the board may go through several
// iterations. When the wiring changes (a pin moves, a power option changes,
// a bus address changes), change these definitions and nothing else — no
// driver or application file should ever contain a raw pin number, bus
// address, or wiring assumption.

// DS18B20 1-Wire temperature sensor bus.
// Every DS18B20 shares this single data pin. The data line MUST have an
// external 4.7 kOhm pull-up resistor to 3V3 — the RP2040's internal pull-up
// (~50 kOhm) is too weak to be relied on, especially with multiple sensors.
#define DS18B20_BUS_PIN 7

// How the DS18B20 sensors are powered on this board revision:
//   0 = external supply (sensor VDD wired to 3V3)
//   1 = parasite power  (sensor VDD wired to GND; power drawn from the data
//       line, which is driven high hard during conversions)
#define DS18B20_PARASITE_POWER 0

// Strong pull-up FET for parasite-power conversions (DS18B20 datasheet
// Figure 6): an AO3401A P-channel FET from 3V3 to the data line, its gate
// driven by this GPIO. The driver holds the FET off at all times except
// during a parasite-power conversion. NOTE: the PCB should include a pull-up
// resistor on the gate so the FET stays off while the RP2040 boots (a
// floating gate can turn a P-FET on and jam the bus high).
#define DS18B20_PULLUP_FET_PIN 8
// AO3401A is P-channel: gate low = FET on. Set to 0 if a future board
// revision drives the FET through an inverting stage.
#define DS18B20_PULLUP_FET_ACTIVE_LOW 1
