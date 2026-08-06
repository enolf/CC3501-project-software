// Simulator implementation of the Pi link. See pi_link.h for the interface and
// why two implementations exist.
//
// This file lets the entire system be exercised with no Raspberry Pi, no
// camera and no network: a shelf of drinks lives in RAM, keystrokes take drinks
// off it, and Square answers on demand. Everything above pi_link.h behaves
// exactly as it will with the real Pi attached.
//
// IT DELIBERATELY IMITATES THE PI'S TIMING, NOT JUST ITS ANSWERS. A scan takes
// a few hundred milliseconds to come back and a Square link takes about a
// second, because instant answers would hide every ordering bug in the state
// machine — the ones that only appear when a reply arrives after the code has
// moved on.

#include "peripherals/pi_link/pi_link.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "pico/stdlib.h"

#include "drivers/logging/logging.h"
#include "peripherals/events/events.h"

namespace pi_link {

namespace {

// --- Simulated timing ---

/// How long the "camera" takes to settle and count after being asked. Chosen
/// well inside checkout::RECOUNT_TIMEOUT_MS so the happy path passes, but long
/// enough that the state machine genuinely has to wait for it.
constexpr uint32_t SIM_SCAN_DELAY_MS = 600;

/// How long Square takes to return a payment link.
constexpr uint32_t SIM_SQUARE_LINK_DELAY_MS = 1000;

/// What a full shelf holds, per drink type.
constexpr uint8_t SIM_FULL_SHELF = 5;

/// Stand-in checkout URL. Deliberately a real, short, harmless address so the
/// QR code rendered from it at stage 9 can actually be scanned with a phone to
/// prove the whole path works.
constexpr const char *SIM_SQUARE_URL = "https://example.com/pay/SIMULATED";

// --- Simulated world state ---

/// What is on the shelf right now. Keystrokes change this; the state machine
/// never sees it directly, only the reports produced by request_scan().
basket::Inventory shelf;

/// The last report handed to the Pi's "camera", i.e. what poll_inventory()
/// will return. Kept separate from `shelf` because the real camera only
/// reports when asked, and only sees a stable scene.
basket::Inventory reported;
bool inventory_waiting = false;

bool     scan_pending = false;
uint32_t scan_due_ms = 0;

bool     square_link_pending = false;
uint32_t square_link_due_ms = 0;
bool     square_url_waiting = false;
bool     square_paid_waiting = false;
bool     square_error_waiting = false;
uint32_t square_amount_cents = 0;

/// The transaction whose link was last cancelled. Remembered only so the 'l'
/// key can produce a late-payment report that names a plausible transaction.
uint32_t square_cancelled_id = 0;

uint32_t now_ms()
{
    return to_ms_since_boot(get_absolute_time());
}

void fill_shelf()
{
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        shelf.count[i] = SIM_FULL_SHELF;
    }
}

void print_shelf()
{
    printf("  simulated shelf:");
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        printf("  %s=%u", catalogue::name(catalogue::from_index(i)),
               shelf.count[i]);
    }
    printf("\n");
}

/// Take one drink off the simulated shelf.
///
/// Note this does NOT report anything to the state machine. That is the point:
/// the real camera only reports on a stable, unobstructed scene, which is why
/// the design waits for the door to close. Drinks vanish silently here and the
/// difference is only discovered by the next scan — exactly as on real
/// hardware.
void take_drink(catalogue::Can can)
{
    const uint8_t index = static_cast<uint8_t>(can);
    if (shelf.count[index] == 0) {
        printf("  %s: none left on the shelf\n", catalogue::name(can));
        return;
    }

    shelf.count[index]--;
    printf("  took a %s (silently - the camera has not looked yet)\n",
           catalogue::name(can));
    print_shelf();
}

} // namespace

void init()
{
    fill_shelf();
    reported = shelf;
    inventory_waiting = false;
    scan_pending = false;
    square_link_pending = false;
    square_url_waiting = false;
    square_paid_waiting = false;
    square_error_waiting = false;

    log(LogLevel::INFORMATION, "pi_link: SIMULATOR backend - no Pi required");
}

void run()
{
    const uint32_t now = now_ms();

    // The "camera" finishes counting.
    if (scan_pending && (int32_t)(now - scan_due_ms) >= 0) {
        scan_pending = false;
        reported = shelf;
        inventory_waiting = true;
        events::push(events::Kind::InventoryUpdated);
    }

    // Square returns a payment link.
    if (square_link_pending && (int32_t)(now - square_link_due_ms) >= 0) {
        square_link_pending = false;
        square_url_waiting = true;
        events::push(events::Kind::SquareUrlReady);
    }
}

void request_scan()
{
    scan_pending = true;
    scan_due_ms = now_ms() + SIM_SCAN_DELAY_MS;
    logf(LogLevel::INFORMATION, "pi_link[sim]: scan requested, replying in %u ms",
         (unsigned)SIM_SCAN_DELAY_MS);
}

void request_square_link(uint32_t cents, uint32_t transaction_id,
                         const basket::Basket &/*basket*/)
{
    // The basket is ignored here on purpose. It exists to name the line item on
    // a real Square receipt, and this backend never reaches Square — the URL it
    // hands back is a fixed literal.
    square_amount_cents = cents;
    square_link_pending = true;
    square_link_due_ms = now_ms() + SIM_SQUARE_LINK_DELAY_MS;
    square_paid_waiting = false;
    square_error_waiting = false;

    logf(LogLevel::INFORMATION,
         "pi_link[sim]: CMD SQUARE_LINK cents=%" PRIu32 " id=%" PRIu32,
         cents, transaction_id);
    printf("  (press 'p' to simulate payment, 'e' to simulate a failure)\n");
}

void request_square_cancel(uint32_t transaction_id)
{
    // The simulator has no real link to kill, so it does the two things that
    // actually matter for testing: it stops a pending link from ever arriving,
    // and it drops any answer already waiting. Without the second part a URL
    // that landed a moment before the back button was pressed would still be
    // collected by the next transaction.
    square_link_pending = false;
    square_url_waiting = false;
    square_paid_waiting = false;
    square_error_waiting = false;
    square_cancelled_id = transaction_id;

    logf(LogLevel::INFORMATION,
         "pi_link[sim]: CMD SQUARE_CANCEL id=%" PRIu32, transaction_id);
    printf("  (press 'l' to simulate the money arriving anyway - a late payment)\n");
}

bool poll_inventory(basket::Inventory &out)
{
    if (!inventory_waiting) {
        return false;
    }
    inventory_waiting = false;
    out = reported;
    return true;
}

bool poll_square_url(char *out, size_t length)
{
    if (!square_url_waiting || out == nullptr || length == 0) {
        return false;
    }
    square_url_waiting = false;

    strncpy(out, SIM_SQUARE_URL, length - 1);
    out[length - 1] = '\0';
    return true;
}

bool poll_square_paid()
{
    if (!square_paid_waiting) {
        return false;
    }
    square_paid_waiting = false;
    return true;
}

bool poll_square_error()
{
    if (!square_error_waiting) {
        return false;
    }
    square_error_waiting = false;
    return true;
}

// --- Outbound notifications ---
//
// The simulator has nowhere to send these, so it prints them in the shape the
// real frames will take (dashboard.md section 6). That makes the protocol
// visible and reviewable long before the transport exists, and means stage 12
// is transcribing something already agreed rather than inventing it.

void notify_sale(const basket::Basket &basket, uint32_t cents,
                 checkout::PaymentMethod method, uint32_t transaction_id)
{
    printf("  -> EVT TXN_START id=%lu items=", (unsigned long)transaction_id);
    bool first = true;
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        if (basket.taken[i] == 0) {
            continue;
        }
        printf("%s%s:%u", first ? "" : ",",
               catalogue::name(catalogue::from_index(i)), basket.taken[i]);
        first = false;
    }
    if (first) {
        printf("(none)");
    }
    printf(" owed=%lu method=%s\n", (unsigned long)cents,
           checkout::payment_method_name(method));
}

void notify_transaction_end(checkout::Outcome outcome, uint32_t owed_cents,
                            uint32_t paid_cents, uint32_t transaction_id)
{
    printf("  -> EVT TXN_END id=%lu outcome=%s owed=%lu paid=%lu\n",
           (unsigned long)transaction_id, checkout::outcome_name(outcome),
           (unsigned long)owed_cents, (unsigned long)paid_cents);
}

void notify_door(bool open)
{
    printf("  -> EVT DOOR state=%s\n", open ? "open" : "closed");
}

void notify_rfid(const uint8_t *uid, uint8_t uid_length, bool granted)
{
    printf("  -> EVT RFID uid=");
    for (uint8_t i = 0; i < uid_length; i++) {
        printf("%02X", uid[i]);
    }
    printf(" granted=%d\n", granted ? 1 : 0);
}

void notify_coin(int32_t cents, double delta_grams, bool accepted)
{
    if (accepted) {
        printf("  -> EVT COIN cents=%ld delta_g=%.2f\n",
               (long)cents, delta_grams);
    } else {
        printf("  -> EVT COIN_REJECT delta_g=%.2f\n", delta_grams);
    }
}

void notify_temperature(uint64_t rom_code, float celsius)
{
    printf("  -> EVT TEMP rom=%08lX%08lX c=%.3f\n",
           (unsigned long)(rom_code >> 32),
           (unsigned long)(rom_code & 0xFFFFFFFFu),
           (double)celsius);
}

void notify_health(float die_celsius, double box_grams, uint32_t faults)
{
    printf("  -> EVT HEALTH die_c=%.1f box_g=%.2f faults=%lu\n",
           (double)die_celsius, box_grams, (unsigned long)faults);
}

bool is_healthy()
{
    // The simulator is, by construction, always reachable. The real backend
    // answers this from how recently a frame arrived.
    return true;
}

bool handle_debug_key(int key)
{
    switch (key) {
        case '1': take_drink(catalogue::Can::Coke);   return true;
        case '2': take_drink(catalogue::Can::Sprite); return true;
        case '3': take_drink(catalogue::Can::Fanta);  return true;
        case '4': take_drink(catalogue::Can::Pasito); return true;

        case 'r':
            fill_shelf();
            printf("  shelf restocked\n");
            print_shelf();
            return true;

        case 'i':
            print_shelf();
            printf("  last reported to the system:");
            for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
                printf("  %s=%u", catalogue::name(catalogue::from_index(i)),
                       reported.count[i]);
            }
            printf("\n");
            return true;

        case 'q':
            // Force the pending Square link to arrive now rather than waiting.
            if (square_link_pending) {
                square_link_due_ms = now_ms();
                printf("  Square link hurried along\n");
            } else {
                printf("  no Square link has been requested\n");
            }
            return true;

        case 'p':
            square_paid_waiting = true;
            events::push(events::Kind::SquarePaid);
            printf("  Square reports payment received (%lu cents)\n",
                   (unsigned long)square_amount_cents);
            return true;

        case 'e':
            square_error_waiting = true;
            square_link_pending = false;
            events::push(events::Kind::SquareError);
            printf("  Square reports a failure\n");
            return true;

        case 'l': {
            // The race that cannot be designed away: the customer's payment and
            // our cancellation cross in flight. The point of this key is to make
            // that path testable on demand rather than hoping to catch it once
            // in a hundred runs.
            events::Event late(events::Kind::SquareLatePaid);
            late.payment.cents = square_amount_cents;
            late.payment.txn_id = square_cancelled_id;
            events::push(late);
            printf("  Square took %lu cents AFTER the link was cancelled"
                   " - a refund is owed\n", (unsigned long)square_amount_cents);
            return true;
        }

        default:
            return false;   // not ours; the caller will try its own keys
    }
}

const char *backend_name()
{
    return "simulator";
}

} // namespace pi_link
