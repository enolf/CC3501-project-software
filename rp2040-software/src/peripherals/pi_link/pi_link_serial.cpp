// Real implementation of the Pi link, over USB CDC. See pi_link.h for the
// interface and pi_link_sim.cpp for the stand-in this replaces.
//
// TRANSPORT
// ---------
// Frames share the USB CDC stream with debug logging (decision D1). That works
// because of two properties of the frame format: every frame begins with EVT,
// CMD or RSP, so a log line is recognised as not-a-frame and dropped in
// silence; and every frame carries a CRC, so a log line landing in the MIDDLE
// of one corrupts it detectably rather than turning it into a plausible wrong
// value. Both are covered by tests in tests/host/test_frame.cpp.
//
// THIS FILE OWNS THE INCOMING BYTE STREAM. Nothing else may call getchar():
// two readers would split the stream between them and every frame would arrive
// with holes. Single-character lines are forwarded to sim_input so the debug
// keys still work, at the cost of needing Enter after them.

#include "peripherals/pi_link/pi_link.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "pico/stdlib.h"

#include "drivers/logging/logging.h"
#include "peripherals/events/events.h"
#include "peripherals/pi_link/frame.h"
#include "peripherals/sim_input/sim_input.h"

namespace pi_link {

namespace {

/// Most bytes to take from the input in one pass.
///
/// Bounded on purpose. An unbounded drain would let a chatty Pi — or a stuck
/// one repeating a character — hold the superloop for as long as it kept
/// talking, which is the one thing nothing here is allowed to do. At 64 bytes
/// a pass and thousands of passes a second this is far more throughput than
/// the link will ever need.
constexpr int MAX_BYTES_PER_PASS = 64;

/// How long without a single valid frame before the link is called unhealthy.
///
/// Generous: the Pi has nothing to say while the fridge is idle, so silence is
/// normal. What this catches is the cable being pulled or `fridged` dying.
constexpr uint32_t LINK_TIMEOUT_MS = 30000;

/// How often to announce that this board is alive.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 10000;

frame::Parser parser;

uint32_t last_frame_ms = 0;
uint32_t next_heartbeat_ms = 0;

// Answers waiting to be collected. Each is cleared by its poll_*(), so every
// answer is delivered exactly once.
basket::Inventory pending_inventory;
bool inventory_waiting = false;

char pending_url[URL_MAX_LEN] = {};
bool url_waiting = false;
bool paid_waiting = false;
bool error_waiting = false;

uint32_t now_ms()
{
    return to_ms_since_boot(get_absolute_time());
}

/// Build a frame and write it out.
void send(frame::Prefix prefix, const char *type, const char *payload)
{
    char wire[frame::MAX_FRAME_LEN];
    const size_t length = frame::build(wire, sizeof(wire), prefix, now_ms(),
                                       type, payload);
    if (length == 0) {
        // Refusing to send is the right failure: a truncated frame that still
        // passed its own CRC would be acted on at the far end.
        logf(LogLevel::ERROR, "pi_link: frame %s too long to send", type);
        return;
    }

    // fwrite rather than printf: the payload is arbitrary text from elsewhere
    // in the program, and handing that to a format-string function is how a
    // stray '%' becomes a crash.
    fwrite(wire, 1, length, stdout);
    fflush(stdout);
}

/// Room for the comma-separated item list two messages both need.
///
/// Sized from the worst real case rather than picked round: four drink types,
/// the longest name being "Sprite", counts that cannot exceed a shelf —
/// "Coke:5,Sprite:5,Fanta:5,Pasito:5" is 32 characters. 40 leaves room and,
/// crucially, leaves the whole payload provably inside frame::MAX_PAYLOAD_LEN
/// once the id, total and method are added.
constexpr size_t ITEM_TEXT_MAX = 40;

/// Render a basket as "Coke:2,Fanta:1". Returns the number of characters
/// written, so an empty basket (0) can be told from a full one.
///
/// Shared by SQUARE_LINK and TXN_START because both name the same sale, and two
/// copies of this loop would be two chances for the receipt and the transaction
/// record to disagree about what was bought.
size_t format_items(const basket::Basket &basket, char *out, size_t length)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        if (basket.taken[i] == 0) {
            continue;
        }
        const int written = snprintf(out + used, length - used, "%s%s:%u",
                                     used > 0 ? "," : "",
                                     catalogue::name(catalogue::from_index(i)),
                                     basket.taken[i]);
        if (written < 0 || (size_t)written >= length - used) {
            break;
        }
        used += (size_t)written;
    }
    return used;
}

/// A complete, CRC-checked frame arrived from the Pi.
void handle_frame(const frame::Frame &f)
{
    last_frame_ms = now_ms();

    if (strcmp(f.type, "INV") == 0) {
        // Absolute counts, not a difference: a missed or corrupted frame then
        // costs one stale reading rather than permanently offsetting the shelf.
        basket::Inventory received;
        bool any = false;
        for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
            uint32_t count = 0;
            // The key is the drink's own name, so adding a drink to the
            // catalogue needs no change here.
            char key[16];
            snprintf(key, sizeof(key), "%s", catalogue::name(catalogue::from_index(i)));
            for (char *c = key; *c != '\0'; c++) {
                if (*c >= 'A' && *c <= 'Z') {
                    *c = (char)(*c - 'A' + 'a');    // the wire uses lower case
                }
            }
            if (frame::u32_for_key(f.payload, key, count)) {
                received.count[i] = (uint8_t)(count > 255 ? 255 : count);
                any = true;
            }
        }

        if (!any) {
            log(LogLevel::WARNING, "pi_link: INV frame carried no drink counts");
            return;
        }

        pending_inventory = received;
        inventory_waiting = true;
        events::push(events::Kind::InventoryUpdated);
        return;
    }

    if (strcmp(f.type, "SQUARE_URL") == 0) {
        if (!frame::value_for_key(f.payload, "url", pending_url,
                                  sizeof(pending_url))) {
            log(LogLevel::WARNING, "pi_link: SQUARE_URL frame had no url");
            error_waiting = true;
            events::push(events::Kind::SquareError);
            return;
        }
        url_waiting = true;
        events::push(events::Kind::SquareUrlReady);
        return;
    }

    if (strcmp(f.type, "SQUARE_PAID") == 0) {
        paid_waiting = true;
        events::push(events::Kind::SquarePaid);
        return;
    }

    if (strcmp(f.type, "SQUARE_ERR") == 0) {
        error_waiting = true;
        events::push(events::Kind::SquareError);
        return;
    }

    if (strcmp(f.type, "SQUARE_CANCELLED") == 0) {
        // Logged, not turned into an event. Nothing in the state machine is
        // waiting on this — the customer was sent back to the payment choice
        // the instant the cancel was sent — so raising an event would only
        // create a message every state has to ignore. What it IS good for is
        // the serial log: `ok=0` means the link is still live and payable, and
        // that is worth being able to find after the fact.
        uint32_t ok = 0;
        uint32_t id = 0;
        (void)frame::u32_for_key(f.payload, "id", id);
        if (frame::u32_for_key(f.payload, "ok", ok) && ok != 0) {
            logf(LogLevel::INFORMATION, "pi_link: payment link for txn %" PRIu32
                 " cancelled", id);
        } else {
            logf(LogLevel::WARNING, "pi_link: payment link for txn %" PRIu32
                 " could NOT be cancelled - it may still be payable", id);
        }
        return;
    }

    if (strcmp(f.type, "SQUARE_LATE_PAID") == 0) {
        // Money arrived after the link was cancelled. Rare, and impossible to
        // rule out entirely: the customer can be mid-payment at the exact
        // moment the cancel is sent. It cannot be handled automatically — the
        // drinks are already gone and the transaction is closed — so it is
        // raised as an event purely to get it recorded where a human will find
        // it. See decision D17.
        events::Event event(events::Kind::SquareLatePaid);
        (void)frame::u32_for_key(f.payload, "cents", event.payment.cents);
        (void)frame::u32_for_key(f.payload, "id", event.payment.txn_id);
        events::push(event);
        return;
    }

    if (strcmp(f.type, "HB") == 0 || strcmp(f.type, "PING") == 0) {
        return;     // liveness only; last_frame_ms above is the whole effect
    }

    logf(LogLevel::WARNING, "pi_link: ignoring unknown frame type '%s'", f.type);
}

// The parser throws away the lines it ignores, so the length of the current
// line is tracked here separately. Only two facts are needed — how many
// characters it held and what the first one was — which is cheaper than keeping
// a second copy of the whole line.
size_t line_length = 0;
char   line_first_char = '\0';

} // namespace

void init()
{
    parser.reset();
    // Counted as if a frame had just arrived, so the link is not declared dead
    // during the first LINK_TIMEOUT_MS while the Pi is still booting.
    last_frame_ms = now_ms();
    next_heartbeat_ms = now_ms() + HEARTBEAT_INTERVAL_MS;

    inventory_waiting = false;
    url_waiting = false;
    paid_waiting = false;
    error_waiting = false;

    log(LogLevel::INFORMATION, "pi_link: SERIAL backend on USB CDC");
    send(frame::Prefix::Event, "BOOT", "fw=0.2");
}

void run()
{
    for (int i = 0; i < MAX_BYTES_PER_PASS; i++) {
        const int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            break;
        }

        const frame::Result result = parser.feed((char)c);

        if (c == '\n') {
            // A line holding exactly one character is a debug keypress, not
            // protocol. This is how the stand-in keys survive on a link that
            // owns stdin — the only difference from the simulator build is that
            // the key needs Enter after it.
            if (result == frame::Result::Ignored && line_length == 1) {
                sim_input::inject((int)line_first_char);
            }
            line_length = 0;
        } else if (c != '\r') {
            if (line_length == 0) {
                line_first_char = (char)c;
            }
            line_length++;
        }

        if (result == frame::Result::Complete) {
            handle_frame(parser.frame);
        }
    }

    const uint32_t now = now_ms();
    if ((int32_t)(now - next_heartbeat_ms) >= 0) {
        next_heartbeat_ms = now + HEARTBEAT_INTERVAL_MS;
        send(frame::Prefix::Event, "HB", nullptr);
    }
}

void request_scan()
{
    send(frame::Prefix::Command, "SCAN", nullptr);
}

void request_square_link(uint32_t cents, uint32_t transaction_id,
                         const basket::Basket &basket)
{
    // The item list contains no spaces — the separator is a comma — which is
    // what lets it be the LAST field in a space-separated payload without the
    // Pi's field parser splitting it across several keys.
    char items[ITEM_TEXT_MAX];
    const size_t used = format_items(basket, items, sizeof(items));

    char payload[frame::MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload),
             "cents=%" PRIu32 " id=%" PRIu32 " desc=%s",
             cents, transaction_id, used > 0 ? items : "Drinks");
    send(frame::Prefix::Command, "SQUARE_LINK", payload);
}

void request_square_cancel(uint32_t transaction_id)
{
    // Any answer still sitting in the pending slots belongs to the checkout
    // being abandoned. Clearing them here stops a URL that arrived a moment too
    // late being handed to the NEXT transaction, which would show a QR code for
    // the wrong amount.
    url_waiting = false;
    paid_waiting = false;
    error_waiting = false;

    char payload[24];
    snprintf(payload, sizeof(payload), "id=%" PRIu32, transaction_id);
    send(frame::Prefix::Command, "SQUARE_CANCEL", payload);
}

bool poll_inventory(basket::Inventory &out)
{
    if (!inventory_waiting) {
        return false;
    }
    inventory_waiting = false;
    out = pending_inventory;
    return true;
}

bool poll_square_url(char *out, size_t length)
{
    if (!url_waiting || out == nullptr || length == 0) {
        return false;
    }
    url_waiting = false;
    strncpy(out, pending_url, length - 1);
    out[length - 1] = '\0';
    return true;
}

bool poll_square_paid()
{
    if (!paid_waiting) {
        return false;
    }
    paid_waiting = false;
    return true;
}

bool poll_square_error()
{
    if (!error_waiting) {
        return false;
    }
    error_waiting = false;
    return true;
}

void notify_sale(const basket::Basket &basket, uint32_t cents,
                 checkout::PaymentMethod method, uint32_t transaction_id)
{
    char items[ITEM_TEXT_MAX];
    const size_t used = format_items(basket, items, sizeof(items));

    char payload[frame::MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload),
             "id=%" PRIu32 " items=%s owed=%" PRIu32 " method=%s",
             transaction_id, used > 0 ? items : "none", cents,
             checkout::payment_method_name(method));
    send(frame::Prefix::Event, "TXN_START", payload);
}

void notify_transaction_end(checkout::Outcome outcome, uint32_t owed_cents,
                            uint32_t paid_cents, uint32_t transaction_id)
{
    char payload[frame::MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload),
             "id=%" PRIu32 " outcome=%s owed=%" PRIu32 " paid=%" PRIu32,
             transaction_id, checkout::outcome_name(outcome), owed_cents,
             paid_cents);
    send(frame::Prefix::Event, "TXN_END", payload);
}

void notify_door(bool open)
{
    send(frame::Prefix::Event, "DOOR", open ? "state=open" : "state=closed");
}

void notify_rfid(const uint8_t *uid, uint8_t uid_length, bool granted)
{
    // The UID is formatted into its own buffer first, then the payload is
    // assembled in one go. Appending the suffix after a variable-length loop
    // works, but leaves the compiler unable to prove the remaining space is
    // enough — and a UID is at most 7 bytes, so bounding it explicitly is both
    // provable and clearer.
    char hex[2 * events::EVENT_UID_MAX_LEN + 1] = {};
    size_t used = 0;
    for (uint8_t i = 0; i < uid_length && i < events::EVENT_UID_MAX_LEN; i++) {
        used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02X", uid[i]);
    }

    char payload[48];
    snprintf(payload, sizeof(payload), "uid=%s granted=%d", hex, granted ? 1 : 0);
    send(frame::Prefix::Event, "RFID", payload);
}

void notify_coin(int32_t cents, double delta_grams, bool accepted)
{
    char payload[48];
    if (accepted) {
        snprintf(payload, sizeof(payload), "denom=%ld delta_g=%.2f",
                 (long)cents, delta_grams);
        send(frame::Prefix::Event, "COIN", payload);
    } else {
        snprintf(payload, sizeof(payload), "delta_g=%.2f", delta_grams);
        send(frame::Prefix::Event, "COIN_REJECT", payload);
    }
}

void notify_temperature(uint64_t rom_code, float celsius)
{
    char payload[48];
    snprintf(payload, sizeof(payload), "rom=%08lX%08lX c=%.3f",
             (unsigned long)(rom_code >> 32),
             (unsigned long)(rom_code & 0xFFFFFFFFu),
             (double)celsius);
    send(frame::Prefix::Event, "TEMP", payload);
}

void notify_health(float die_celsius, double box_grams, uint32_t faults)
{
    char payload[64];
    snprintf(payload, sizeof(payload), "die_c=%.1f box_g=%.2f faults=%" PRIu32,
             (double)die_celsius, box_grams, faults);
    send(frame::Prefix::Event, "HEALTH", payload);
}

bool is_healthy()
{
    // Judged purely on how recently ANY valid frame arrived, which is why the
    // Pi is expected to send its own HB every 10 s. Without that, a link that
    // is perfectly fine would be declared dead simply because the fridge had
    // been quiet — the Pi has nothing to say while nobody is buying anything.
    return (now_ms() - last_frame_ms) < LINK_TIMEOUT_MS;
}

bool handle_debug_key(int /*key*/)
{
    // The real link has no simulated behaviour to trigger. Keys forwarded from
    // handle_ignored_line() go to the local handler in main.cpp instead.
    return false;
}

const char *backend_name()
{
    return "serial";
}

} // namespace pi_link
