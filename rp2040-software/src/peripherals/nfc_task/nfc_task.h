#pragma once

#include <stdbool.h>
#include <stdint.h>

// The RFID reader: turns a card tap into an event.
//
// An approved card is a WAKE TRIGGER and nothing more (decision D9). It starts
// a transaction exactly as opening the door does, and the customer still pays
// by cash or QR. It is not a payment method and it does not unlock anything —
// there is no lock yet, and dashboard.md section 9 explains why attributing
// purchases to cards before there is one would be unfair.

namespace nfc {

/// Bring up the reader.
///
/// Returns false if the chip does not answer or reports the wrong version. That
/// is NOT fatal: run_nfc() keeps retrying in the background, so a reader that is
/// slow to power up, or on an intermittent connection, recovers on its own
/// without a reset.
bool init();

/// Poll for a card. Call every pass of the superloop.
///
/// COSTS UP TO ABOUT 25 ms WHEN IT POLLS. The MFRC522 is asked "is any card
/// there?" and answers by running its own timer to expiry when the answer is
/// no, which the driver waits out. That is the largest single stall in the
/// system, so this deliberately polls only every NFC_POLL_INTERVAL_MS rather
/// than every pass, and does nothing at all in between.
void run_nfc();

/// True once the reader has answered and been configured.
bool is_ready();

/// Scan the RFID I2C bus and print every address that acknowledges.
///
/// A diagnostic, not part of normal operation. When the reader will not
/// initialise this is the first thing to run: it separates "nothing on the bus
/// at all" (wiring, power, pull-ups) from "something is there but not at the
/// address we expect" (the module's ASW address switches), which are entirely
/// different faults with entirely different fixes.
///
/// BLOCKS for a few milliseconds while it sweeps all 112 valid addresses.
void scan_bus();

} // namespace nfc
