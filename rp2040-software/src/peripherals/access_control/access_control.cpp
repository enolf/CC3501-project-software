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
// THE APPROVED LIST
//
// Lives in access_list.h, which is GITIGNORED, because a card UID paired with a
// name identifies a real person. Unlike a password a UID cannot be rotated — it
// is burned into the card at manufacture — so the only remedy for a leaked one
// is issuing a new card. Same arrangement as `tokens.py` on the Pi side: the
// secret is created by hand on each machine and only a template is committed.
//
// `approved_users[]` is defined there, still as a plain array of ApprovedUser,
// so everything below continues to size itself from it.
//
// Included HERE rather than at the top of the file because the definition needs
// the ApprovedUser type declared above it. Keeping the two adjacent is what
// makes that ordering obvious to whoever edits it next.
// ---------------------------------------------------------------------------
#if __has_include("peripherals/access_control/access_list.h")
#include "peripherals/access_control/access_list.h"
#else
// A missing file would otherwise surface as "approved_users was not declared",
// which says nothing about what to do. This says it.
#error "access_list.h is missing. It holds the approved card UIDs and is gitignored on purpose, so it does not arrive with a clone and must be written by hand. documentation.md section 3.5 has the file to copy."
#endif

/// How many rows the table above has. Everything else sizes itself from this,
/// so adding or removing a person needs no other change.
constexpr size_t APPROVED_COUNT =
    sizeof(approved_users) / sizeof(approved_users[0]);

const char *access_lookup(const uint8_t *uid, uint8_t uid_len)
{
    const size_t count = APPROVED_COUNT;
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

bool access_first(uint8_t *uid_out, uint8_t *uid_len_out)
{
    // An empty approved list is a legitimate state — a fridge nobody has been
    // enrolled on yet — so this reports "nothing to give" rather than asserting.
    if (APPROVED_COUNT == 0) {
        return false;
    }

    memcpy(uid_out, approved_users[0].uid, approved_users[0].uid_len);
    *uid_len_out = approved_users[0].uid_len;
    return true;
}
