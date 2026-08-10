#include "peripherals/nfc_task/nfc_task.h"

#include <stdio.h>
#include <string.h>

#include "timings.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "board.h"
#include "drivers/logging/logging.h"
#include "drivers/mfrc522/mfrc522.h"
#include "peripherals/access_control/access_control.h"
#include "peripherals/events/events.h"

namespace nfc {

namespace {



bool ready = false;

// Remembering the last card is what stops a card resting on the pad from being
// reported dozens of times a second: cards keep answering the "anyone there?"
// query for as long as they are in the field.
uint8_t last_uid[MFRC522_UID_MAX_LEN] = {};
uint8_t last_uid_len = 0;
bool    card_was_present = false;

uint32_t now_ms()
{
    return to_ms_since_boot(get_absolute_time());
}

void report(const uint8_t *uid, uint8_t uid_len, const char *holder)
{
    events::Event e(holder != nullptr ? events::Kind::CardApproved
                                      : events::Kind::CardDenied);
    const uint8_t copy_len = uid_len < events::EVENT_UID_MAX_LEN
                                 ? uid_len : events::EVENT_UID_MAX_LEN;
    memcpy(e.card.uid, uid, copy_len);
    e.card.len = copy_len;
    events::push(e);

    char text[3 * MFRC522_UID_MAX_LEN + 1] = {};
    for (uint8_t i = 0; i < uid_len && i < MFRC522_UID_MAX_LEN; i++) {
        snprintf(text + (i * 2), sizeof(text) - (i * 2), "%02X", uid[i]);
    }

    if (holder != nullptr) {
        logf(LogLevel::INFORMATION, "nfc: %s approved (%s)", text, holder);
    } else {
        logf(LogLevel::WARNING, "nfc: %s is not on the approved list", text);
    }
}

} // namespace

bool init()
{
    ready = mfrc522_init();
    if (ready) {
        log(LogLevel::INFORMATION, "nfc: MFRC522 ready");
    } else {
        logf(LogLevel::ERROR,
             "nfc: MFRC522 not found on i2c1 (SDA GP%d, SCL GP%d, addr 0x%02X)"
             " - will keep retrying; press 'I' to scan the bus",
             RFID_SDA_PIN, RFID_SCL_PIN, RFID_I2C_ADDR);
    }
    return ready;
}

void run_nfc()
{
    static uint32_t next_poll_ms = 0;
    static uint32_t next_retry_ms = timings::NFC_RETRY_INTERVAL_MS;

    const uint32_t now = now_ms();

    // --- Not initialised: retry occasionally, do nothing else ---
    if (!ready) {
        if ((int32_t)(now - next_retry_ms) < 0) {
            return;
        }
        next_retry_ms = now + timings::NFC_RETRY_INTERVAL_MS;

        if (mfrc522_init()) {
            ready = true;
            log(LogLevel::INFORMATION, "nfc: MFRC522 appeared and initialised");
        }
        return;
    }

    if ((int32_t)(now - next_poll_ms) < 0) {
        return;
    }
    next_poll_ms = now + timings::NFC_POLL_INTERVAL_MS;

    uint8_t uid[MFRC522_UID_MAX_LEN];
    uint8_t uid_len = 0;

    if (!mfrc522_card_present() || !mfrc522_read_card_uid(uid, &uid_len)) {
        // No card, or a read that failed part way. Either way the pad is
        // considered empty, so the next successful read counts as a new tap.
        card_was_present = false;
        return;
    }

    const bool same_card = card_was_present &&
                           uid_len == last_uid_len &&
                           memcmp(uid, last_uid, uid_len) == 0;
    card_was_present = true;

    if (same_card) {
        return;     // still the same card sitting there
    }

    memcpy(last_uid, uid, uid_len);
    last_uid_len = uid_len;

    report(uid, uid_len, access_lookup(uid, uid_len));
}

bool is_ready()
{
    return ready;
}

void scan_bus()
{
    printf("\n--- I2C scan on the RFID bus (SDA GP%d, SCL GP%d) ---\n",
           RFID_SDA_PIN, RFID_SCL_PIN);

    // The driver configures the bus during its own init, which may never have
    // run if that failed. Set it up here so the scan works regardless.
    i2c_init(RFID_I2C_INSTANCE, RFID_I2C_BAUDRATE);
    gpio_set_function(RFID_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(RFID_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(RFID_SDA_PIN);
    gpio_pull_up(RFID_SCL_PIN);

    int found = 0;
    // 0x00-0x07 and 0x78-0x7F are reserved by the I2C specification and are
    // never valid device addresses, so they are skipped.
    for (uint8_t address = 0x08; address < 0x78; address++) {
        uint8_t discard = 0;
        const int result = i2c_read_blocking(RFID_I2C_INSTANCE, address,
                                             &discard, 1, false);
        if (result >= 0) {
            printf("  0x%02X  responded%s\n", address,
                   address == RFID_I2C_ADDR ? "   <-- expected RFID address" : "");
            found++;
        }
    }

    if (found == 0) {
        printf("  nothing responded.\n");
        printf("  The pins above are confirmed against the PCB, so suspect the\n");
        printf("  module or the cable, not board.h. Check in this order: 3V3 and\n");
        printf("  GND actually present at the module, SDA/SCL not swapped in the\n");
        printf("  cable, and pull-ups present (the PiicoDev module has its own,\n");
        printf("  so a module that is not powered takes them away with it).\n");
    } else {
        printf("  %d device(s) found.\n", found);
        printf("  If the RFID module answered at an address other than 0x%02X,\n",
               RFID_I2C_ADDR);
        printf("  its ASW address switches are set differently - change\n");
        printf("  RFID_I2C_ADDR in board.h to match.\n");
    }
    printf("\n");
}

} // namespace nfc
