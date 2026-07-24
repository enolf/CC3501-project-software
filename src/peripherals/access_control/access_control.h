#pragma once

#include <stdint.h>

// Access control for the RFID fridge lock.
//
// Maps approved card UIDs to the holder's name. The scanning side (main) reads
// a UID from the reader and asks access_lookup() whether it is allowed; the
// answer — a name, or "not approved" — is what the TFT display will show.
//
// This module is pure logic: it knows nothing about the reader or the display,
// only about the policy (which UID belongs to whom). Edit the approved list in
// access_control.cpp.

// Look up a scanned card UID against the approved list.
//   uid     : the UID bytes from mfrc522_read_card_uid()
//   uid_len : how many bytes are valid (4 or 7)
// Returns the holder's name if the UID is approved, or nullptr if it is not.
// The returned string is a compile-time constant — safe to hold and print
// (e.g. to the TFT); never free it.
const char *access_lookup(const uint8_t *uid, uint8_t uid_len);
