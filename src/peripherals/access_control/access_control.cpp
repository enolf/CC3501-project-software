// Access control policy: the list of approved cards and the UID lookup.

#include "access_control.h"

#include <stddef.h>
#include <string.h>
#include "drivers/mfrc522/mfrc522.h"   // MFRC522_UID_MAX_LEN

// One approved card: the holder's name plus the UID that identifies them.
// uid_len is 4 for single-size UIDs (e.g. Mifare Classic) or 7 for double-size
// (NTAG, Mifare Ultralight, DESFire). Only the first uid_len bytes are compared,
// so a 4-byte entry can leave the remaining array slots at zero.
typedef struct {
    const char *name;
    uint8_t     uid_len;
    uint8_t     uid[MFRC522_UID_MAX_LEN];
} ApprovedUser;

// ---------------------------------------------------------------------------
// THE APPROVED LIST  ("the database")
//
// To add someone: place their card on the reader and read the serial line
//     Card detected, UID: AA BB CC DD EE FF 00
// then copy those bytes into a new row below, set uid_len to the number of
// bytes, and give them a name. To revoke access, delete their row. Nothing
// else needs to change — access_lookup() sizes itself from this table.
//
// The UIDs below are PLACEHOLDERS — replace them with your real cards.
// ---------------------------------------------------------------------------
static const ApprovedUser approved_users[] = {
    { "Damien Turner", 7, { 0x04, 0x40, 0x4C, 0x22, 0xC0, 0x67, 0x80 } },

    // Add more people the same way — one row each, then re-build. For example:
    // { "Jane Smith", 7, { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } },
    // { "Spare Fob",  4, { 0xDE, 0xAD, 0xBE, 0xEF } },
};

const char *access_lookup(const uint8_t *uid, uint8_t uid_len)
{
    const size_t count = sizeof(approved_users) / sizeof(approved_users[0]);
    for (size_t i = 0; i < count; i++) {
        // A match needs the same length AND the same bytes. Checking the length
        // first means a 4-byte card can never accidentally match the first four
        // bytes of a 7-byte entry.
        if (approved_users[i].uid_len == uid_len &&
            memcmp(approved_users[i].uid, uid, uid_len) == 0) {
            return approved_users[i].name;
        }
    }
    return nullptr;   // not on the list -> access denied
}
