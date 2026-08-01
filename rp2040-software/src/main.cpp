// Smart Fridge Drink Monitor — firmware entry point.
//
// This file holds top-level control flow and nothing else: bring the hardware
// up, then call each task once per pass, forever. How any individual device
// works belongs in its driver; what the system does with it belongs in a task.
//
// THE ONE RULE FOR EVERYTHING CALLED FROM HERE
// --------------------------------------------
// Every run_*() function must return promptly. No sleep_ms(), no waiting for a
// sensor, no blocking bus transaction. A task that needs to wait remembers
// where it got to in a `static` local and returns, and picks up on the next
// pass. The whole system shares one thread of execution, so anything that
// blocks freezes the display, stops coins being counted, and stalls the link
// to the Pi at the same time.
//
// The previous contents of this file — one standalone main() per peripheral —
// are preserved in docs/bringup/bringup_examples.cpp.txt.

#include <stdio.h>
#include <inttypes.h>
#include "pico/stdlib.h"

#include "board.h"
#include "sim_config.h"
#include "drivers/logging/logging.h"
#include "peripherals/events/events.h"
#include "peripherals/checkout/checkout.h"
#include "peripherals/switches/switches.h"
#include "peripherals/tft_display/tft_display.h"
#include "drivers/sd_card/sd_card.h"
#include "peripherals/sd_log/sd_log.h"
#include "peripherals/pi_link/pi_link.h"
#include "peripherals/sim_input/sim_input.h"
#include "peripherals/scale_task/scale_task.h"
#include "peripherals/nfc_task/nfc_task.h"
#include "peripherals/temperature_task/temperature_task.h"

namespace {

/// Reported in the BOOT line so a log can be matched to a build.
constexpr const char *FIRMWARE_VERSION = "0.2";

/// How often the "still alive" line is printed. Its real job is negative
/// evidence: a heartbeat that stops, or arrives late, means something in the
/// loop has blocked, and that is the failure this project most needs to catch.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 10000;

/// Longest single pass through the superloop that is considered acceptable.
/// LVGL wants servicing every few milliseconds, so anything approaching this is
/// already a problem worth naming.
constexpr uint32_t LOOP_TIME_WARN_MS = 50;

/// How often the board reports its own condition to the Pi. Matched to the
/// temperature sample rate so the dashboard gets one coherent picture per
/// interval rather than two that disagree by a few seconds.
constexpr uint32_t HEALTH_INTERVAL_MS = 30000;

// ---------------------------------------------------------------------------
// Heartbeat and loop-time watchdog
// ---------------------------------------------------------------------------
// Not a task in the real sense — it observes the loop rather than any hardware.
// It becomes the source of the protocol's HB frame at stage 12.
void run_heartbeat()
{
    // Static locals are how a non-blocking task remembers where it was between
    // calls. `next_beat` survives from one pass to the next; the whole function
    // does nothing at all on the vast majority of passes.
    static absolute_time_t next_beat = make_timeout_time_ms(HEARTBEAT_INTERVAL_MS);
    static uint32_t worst_loop_ms = 0;
    static absolute_time_t last_pass = get_absolute_time();

    // Measure how long the previous pass through the whole superloop took.
    absolute_time_t now = get_absolute_time();
    uint32_t pass_ms = (uint32_t)(absolute_time_diff_us(last_pass, now) / 1000);
    last_pass = now;
    if (pass_ms > worst_loop_ms) {
        worst_loop_ms = pass_ms;
    }
    if (pass_ms > LOOP_TIME_WARN_MS) {
        logf(LogLevel::WARNING, "loop: pass took %" PRIu32 " ms - something is blocking",
             pass_ms);
    }

    if (!time_reached(next_beat)) {
        return;
    }
    next_beat = make_timeout_time_ms(HEARTBEAT_INTERVAL_MS);

    logf(LogLevel::INFORMATION,
         "HB  worst_loop=%" PRIu32 " ms  queued=%u  dropped=%" PRIu32,
         worst_loop_ms, events::count(), events::dropped());

    // Report the worst case per interval rather than since boot, so a stall is
    // visible in the interval it happened in instead of being remembered
    // forever by a single early outlier.
    worst_loop_ms = 0;
}

/// Send the periodic HEALTH frame.
///
/// Assembled here rather than inside any one task because it deliberately spans
/// three of them — the die temperature, the coin box, and the fault count — and
/// main is where top-level composition belongs. None of those modules should
/// have to know about the others.
void run_health_report()
{
    static absolute_time_t next = make_timeout_time_ms(HEALTH_INTERVAL_MS);

    if (!time_reached(next)) {
        return;
    }
    next = make_timeout_time_ms(HEALTH_INTERVAL_MS);

    pi_link::notify_health(temperature::die_celsius(),
                           scale::level_grams(),
                           checkout::fault_count());
}

// ---------------------------------------------------------------------------
// Debug keyboard
// ---------------------------------------------------------------------------
// Turns keystrokes on the USB serial monitor into events, so the system can be
// exercised with no peripherals attached. Stage 4 moves this into the pi_link
// simulator; for now it exists to prove the queue works end to end.
void print_help()
{
    printf("\n--- debug keys ---\n");
    printf(" Pi simulator (stands in for the camera and Square):\n");
    printf("  1 2 3 4  take a Coke / Sprite / Fanta / Pasito off the shelf\n");
    printf("  r        restock the shelf\n");
    printf("  i        show the shelf, and what was last reported\n");
    printf("  q        hurry along a pending Square link\n");
    printf("  p        Square reports the payment received\n");
    printf("  e        Square reports a failure\n");
    printf(" Hardware and stand-ins:\n");
#if SIM_DOOR
    printf("  o / c    FAKE door opened / closed (the real switch works too)\n");
#endif
    printf("  b        user button pressed\n");
#if SIM_NFC
    printf("  n        FAKE approved card tapped\n");
#endif
#if SIM_TOUCH
    printf(" Touch stand-ins (the real touchscreen works too):\n");
    printf("  ,        tap CASH\n");
    printf("  .        tap ONLINE\n");
    printf("  /        tap PAY CASH INSTEAD (back)\n");
#endif
#if SIM_COINS
    printf("  5 / 6    FAKE $1 / $2 coin (the real scale works too)\n");
    printf("  7        FAKE rejected coin\n");
#endif
    printf("  f        fault\n");
    printf(" Coin scale:\n");
    printf("  g        toggle raw gram dump (for the noise floor and weighing)\n");
    printf("  t        re-tare the scale (BLOCKS ~1 s; keep the box still)\n");
    printf(" Diagnostics:\n");
    printf("  S        request a shelf scan, as the door closing will\n");
    printf("  L        request a Square payment link ($2.00)\n");
    printf("  T        preview the outbound frames (sale, end, temperature)\n");
    printf("  !        flood the queue, to prove overflow is handled\n");
    printf("  s        show queue statistics\n");
    printf("  d        probe the SD card (BLOCKS for up to ~1 s)\n");
    printf("  w        append a test line to LOG.TXT on the card (BLOCKS)\n");
    printf("  ?        this help\n\n");
}

// ---------------------------------------------------------------------------
// SD card probe — feasibility spike, run on demand only
// ---------------------------------------------------------------------------
// Deliberately NOT part of the superloop. The card's initialisation handshake
// is polled and the specification allows it to take up to a second, which would
// freeze the display and trip the loop-time warning. Running it from a keypress
// makes the cost visible and voluntary.
void probe_sd_card()
{
    printf("\n--- SD card probe ---\n");

    SdCardInfo info;
    if (!sd_card_probe(info)) {
        printf("  no card detected (empty socket, or wiring/bus problem)\n\n");
        return;
    }

    printf("  type      : %s\n", sd_card_type_name(info.type));
    printf("  product   : %s\n", info.product_name);
    printf("  made      : %04u-%02u\n", info.manufacture_year, info.manufacture_month);
    // Printed as a 32-bit megabyte count rather than a 64-bit byte count: the
    // SDK's default printf is newlib-nano, whose long-long support is not
    // compiled in, so a %llu here can silently print nothing. Megabytes fit in
    // 32 bits for any card up to 4 TB.
    printf("  capacity  : %" PRIu32 " MB\n",
           (uint32_t)(info.capacity_bytes / (1024ULL * 1024ULL)));

    // Reading block 0 proves data transfer works, not just the handshake. On a
    // formatted card block 0 is the master boot record, which ends in the
    // signature 0x55 0xAA — a cheap, specific check that the bytes arriving are
    // real rather than a floating bus reading as all-ones.
    uint8_t block[512];
    if (!sd_card_read_block(0, block)) {
        printf("  block 0   : READ FAILED\n\n");
        return;
    }

    printf("  block 0   : read ok, first bytes %02X %02X %02X %02X\n",
           block[0], block[1], block[2], block[3]);

    if (block[510] == 0x55 && block[511] == 0xAA) {
        printf("  signature : 0x55AA present - card is formatted\n");
    } else {
        printf("  signature : absent (%02X %02X) - card may be unformatted\n",
               block[510], block[511]);
    }
    printf("\n");
}

void run_debug_input()
{
    int key;
    if (!sim_input::poll(key)) {
        return;
    }

    // The Pi simulator gets first refusal, because the keys standing in for the
    // camera and for Square belong to it. Anything it does not recognise falls
    // through to the local keys below. With the real serial backend compiled,
    // handle_debug_key() always returns false and every key lands here.
    if (pi_link::handle_debug_key(key)) {
        return;
    }

    switch (key) {
        case 'b': events::push(events::Kind::UserButtonPressed); break;

#if SIM_DOOR
        case 'o': events::push(events::Kind::DoorOpened); break;
        case 'c': events::push(events::Kind::DoorClosed); break;
#endif

#if SIM_TOUCH
        // Stand-ins for the touchscreen, so the whole flow can be walked
        // without touching the glass.
        case ',': events::push(events::Kind::TouchCash);   break;
        case '.': events::push(events::Kind::TouchOnline); break;
        case '/': events::push(events::Kind::TouchBack);   break;
#endif

        case 'I':
            // Diagnostic for a reader that will not initialise. Separates
            // "nothing on the bus" from "something at a different address".
            nfc::scan_bus();
            break;

        case 'S':
            // What the state machine will do when the door closes. Useful on
            // its own for watching the request/reply round trip.
            pi_link::request_scan();
            break;

        case 'L':
            // Ask for a Square payment link, as choosing "Online" will.
            pi_link::request_square_link(200, 1);
            break;

        case 'T': {
            // Preview the outbound frames the state machine will send once it
            // exists. Nothing here is a real transaction; the point is to make
            // the protocol visible and reviewable now, so stage 12 transcribes
            // something already agreed rather than inventing it.
            printf("\n--- outbound frame preview (not a real sale) ---\n");
            basket::Basket example;
            example.taken[static_cast<uint8_t>(catalogue::Can::Coke)] = 2;
            example.taken[static_cast<uint8_t>(catalogue::Can::Fanta)] = 1;
            const uint32_t owed = basket::total_cents(example);

            pi_link::notify_sale(example, owed, checkout::PaymentMethod::Cash, 1);
            pi_link::notify_transaction_end(checkout::Outcome::Paid, owed, owed, 1);
            pi_link::notify_temperature(0x28FF1234567890ABull, 4.25f);
            printf("\n");
            break;
        }

#if SIM_NFC
        case 'n': {
            events::Event e(events::Kind::CardApproved);
            // A plausible 4-byte UID, so the payload path is exercised too.
            e.card.uid[0] = 0xDE; e.card.uid[1] = 0xAD;
            e.card.uid[2] = 0xBE; e.card.uid[3] = 0xEF;
            e.card.len = 4;
            events::push(e);
            break;
        }
#endif // SIM_NFC

        case 'g':
            scale::set_raw_dump(!scale::raw_dump_enabled());
            printf("raw gram dump %s\n",
                   scale::raw_dump_enabled() ? "ON" : "OFF");
            break;

        case 't':
            // Blocks for about a second, so only allowed when nothing is in
            // progress. Mid-transaction it would freeze the screen and move the
            // zero the customer's coins are being measured against.
            if (checkout::current() != checkout::State::Idle) {
                printf("re-tare refused: only while idle\n");
                break;
            }
            printf("re-taring, keep the money box still...\n");
            printf(scale::retare() ? "tared\n" : "TARE FAILED\n");
            break;

#if SIM_COINS
        case '5':
        case '6': {
            events::Event e(events::Kind::CoinAccepted);
            const bool is_two = (key == '6');
            e.coin.one_dollar  = is_two ? 0 : 1;
            e.coin.two_dollar  = is_two ? 1 : 0;
            e.coin.cents       = is_two ? 200 : 100;
            e.coin.delta_grams = is_two ? 6.60f : 9.80f;
            events::push(e);
            break;
        }

        case '7': {
            events::Event e(events::Kind::CoinRejected);
            e.coin.delta_grams = 4.0f;   // a washer, say
            events::push(e);
            break;
        }
#endif // SIM_COINS

        case 'f': {
            events::Event e(events::Kind::Fault);
            e.fault.code = 1;
            events::push(e);
            break;
        }

        case '!':
            // Deliberately push more than the queue can hold. The queue should
            // log one error, drop the excess, and carry on — not corrupt itself.
            printf("flooding with %u events (capacity %u)...\n",
                   events::QUEUE_CAPACITY * 2, events::QUEUE_CAPACITY);
            for (uint8_t i = 0; i < events::QUEUE_CAPACITY * 2; i++) {
                events::push(events::Kind::DoorOpened);
            }
            break;

        case 's':
            printf("queue: %u waiting, %" PRIu32 " dropped since boot\n",
                   events::count(), events::dropped());
            break;

        case 'd':
            probe_sd_card();
            break;

        case 'w': {
            if (!sd_log::is_available()) {
                printf("SD log is not mounted - no card, or not FAT formatted.\n");
                break;
            }
            static uint32_t test_lines = 0;
            test_lines++;
            const bool ok = sd_log::write_linef(
                "test line %" PRIu32 " - door is %s", test_lines,
                switches::is_door_closed() ? "closed" : "open");
            printf("SD log: %s (%lu bytes written this session)\n",
                   ok ? "line appended" : "WRITE FAILED", sd_log::bytes_written());
            break;
        }

        case '?':
            print_help();
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Temporary event consumer
// ---------------------------------------------------------------------------
// Stands in for run_checkout() until stage 5. It drains the queue and reports
// what it found, which is all that is needed to prove events flow from producer
// to consumer within a single pass. Stage 5 deletes this and replaces the call
// below with the state machine.
} // namespace

int main()
{
    // Open the debug log channel over USB CDC. Deliberately NOT waiting for a
    // monitor to attach: the fridge must run the instant it has power, and the
    // serial log is an optional debugging window. Anything printed before a
    // terminal connects is simply missed.
    stdio_init_all();

    logf(LogLevel::INFORMATION, "BOOT fw=%s pi_link=%s", FIRMWARE_VERSION,
         pi_link::backend_name());

    // Say plainly which kind of build this is. With the stand-ins compiled in,
    // anyone who can reach the USB socket can fake a paid transaction by typing
    // a character, so a build that has them must never be mistaken for one that
    // does not.
#if SIM_ANY
    logf(LogLevel::WARNING,
         "BENCH BUILD - keyboard stand-ins active (door=%d nfc=%d coins=%d touch=%d)",
         SIM_DOOR, SIM_NFC, SIM_COINS, SIM_TOUCH);
    logf(LogLevel::WARNING,
         "  do not leave this build in the fridge: the debug keys can fake payment");
#else
    log(LogLevel::INFORMATION, "DEPLOYMENT BUILD - no keyboard stand-ins");
#endif
    logf(LogLevel::INFORMATION, "state machine starts in %s",
         checkout::state_name(checkout::State::Idle));

    switches::init();

    // Park the SD card's chip select high before anything drives the SPI bus.
    // The card shares SCK/MOSI/MISO with the display and touch controller, so
    // an un-driven select could leave it contending for MISO from the first
    // display transaction onward.
    sd_card_init_pins();

    // Bring up the link to the Pi. Which implementation this is was decided at
    // compile time by PI_LINK_BACKEND; nothing below here can tell.
    pi_link::init();

    // Bring up the coin scale. Blocks for about a second while it tares, which
    // is why it happens here and not on the first coin. A failure is logged and
    // survived: everything except cash payment still works.
    scale::init();

    // Bring up the RFID reader. A failure here is logged and survived: the task
    // keeps retrying in the background, and the door remains a perfectly good
    // way to start a transaction meanwhile.
    nfc::init();

    // Bring up the temperature sensors and the RP2040's own die sensor.
    temperature::init();

    // Bring the display up. This blocks for roughly 750 ms in the ILI9341's
    // reset and wake sequence, which the datasheet requires and which is fine
    // here: startup is the one place blocking is allowed. Nothing inside the
    // loop below may do the same.
    //
    // Safe to call with no panel attached — SPI is write-only here, so the
    // transactions simply go nowhere and the rest of the system runs normally.
    Display::init();

    // Mount the SD log and record that the board started. Deliberately after
    // the display, so a slow or faulty card cannot delay the screen coming up,
    // and deliberately non-fatal: if there is no card the fridge trades exactly
    // as before, with logging going to the serial monitor only.
    if (sd_log::init()) {
        sd_log::write_linef("BOOT fw=%s door=%s", FIRMWARE_VERSION,
                            switches::is_door_closed() ? "closed" : "open");
    }

    // Start the state machine last, so it draws the idle screen over whatever
    // startup produced and everything it depends on is already up.
    checkout::init();

    print_help();

    for (;;) {
        // Producers first, then the consumer, so an event raised this pass is
        // handled on the same pass rather than waiting for the next one.
        run_debug_input();
        switches::run_switches();
        scale::run_scale();     // raises coin events from the load cell
        nfc::run_nfc();             // raises card events (self-throttling)
        temperature::run_temperature();   // samples every 30 s, never blocks
        pi_link::run();         // delivers scan results and Square replies

        // The state machine: the only consumer of the event queue, and the only
        // code that decides what any of it means.
        checkout::run_checkout();

        // Service LVGL: redraws, animations and touch sampling. Wants calling
        // every few milliseconds, which is the real reason nothing above may
        // block.
        Display::run();

        run_health_report();
        run_heartbeat();
    }

    return 0;
}
