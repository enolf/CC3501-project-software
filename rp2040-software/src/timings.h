#pragma once

#include <stdint.h>

// Every interval that shapes how the system FEELS to use, in one file.
//
// WHY THIS FILE EXISTS
// --------------------
// `board.h` is the single place a wiring fact may appear, so a hardware change
// is one edit rather than a hunt. This is the same rule for time. Tuning the
// flow — how long a screen lingers, how long a customer gets to find coins, how
// quickly a card tap registers — used to mean editing five files in three
// directories, and the numbers that had to agree with each other were nowhere
// near each other.
//
// **If a flow interval appears anywhere else in `src/`, that is the bug.**
//
// WHAT BELONGS HERE, AND WHAT DOES NOT
// ------------------------------------
// Here: anything you would change to make the system feel different. Screen
// dwell times, state timeouts, poll rates, report cadences.
//
// NOT here: timing a device's datasheet dictates. The DS18B20's 480 us reset
// pulse, the SD card's SPI timeouts and DigitalSwitch's debounce window are not
// tuning knobs — they are properties of the part, and moving them away from the
// driver that explains them would invite someone to "tune" a value that is
// actually a specification. They stay with their drivers.
//
// UNITS ARE IN THE NAME
// ---------------------
// Every constant ends in _MS and every one is milliseconds. A bare number here
// with the wrong unit would be a silent multiplication error, and this file is
// exactly where somebody edits quickly without reading.

namespace timings {

// --- Customer-facing screens ------------------------------------------------
// Starting points to be tuned on the bench, not measurements. Every state that
// can be entered must be able to leave on its own, or a customer walking away
// would strand the terminal until someone opened the door.

/// Selecting -> Idle. Covers a door left standing open.
constexpr uint32_t SELECT_TIMEOUT_MS = 30000;

/// Greeting -> Idle. Someone tapped their card and then walked away, or tapped
/// it out of curiosity. Generous compared with the 3 s screens below because a
/// person who has just identified themselves is usually about to open the door,
/// and cutting the greeting short mid-reach reads as the terminal ignoring them.
constexpr uint32_t GREETING_TIMEOUT_MS = 30000;

/// AccessDenied -> Idle. Long enough to read a two-line message, short enough
/// that a rejected card cannot leave a red screen up for the next customer.
constexpr uint32_t DENIED_MS = 3000;

/// Recount -> Fault. How long the Pi gets to stabilise and report.
///
/// Must stay comfortably above however long `fridged` takes to answer a
/// `CMD SCAN`. It is a fault rather than a fallback because with no inventory
/// there is no way to know what to charge for.
constexpr uint32_t RECOUNT_TIMEOUT_MS = 8000;

/// Any payment state -> Abandoned. Restarted by every accepted coin and every
/// touch, so a customer slowly finding coins is never cut off mid-payment.
///
/// Also the window a customer has to scan the QR and complete a card payment,
/// so shortening this makes online payment harder, not just cash.
constexpr uint32_t PAYMENT_TIMEOUT_MS = 120000;

/// PayOnlineLink -> PaymentSelect. A payment link that does not arrive falls
/// back to the payment choice, so cash is still available; it is not a fault.
///
/// TUNE THIS AGAINST THE PI, NOT ON ITS OWN. `square.py` gives each HTTP call
/// `HTTP_TIMEOUT_S = 15`, so a slow-but-successful Square call currently loses
/// this race and the customer is bounced to the payment choice. That is safe —
/// leaving this state fires `CMD SQUARE_CANCEL`, and the Pi applies it even to a
/// link that has not been created yet — but on a flaky connection you want this
/// number above the Pi's.
constexpr uint32_t SQUARE_LINK_TIMEOUT_MS = 10000;

/// How long the confirmation screen stays up before returning to Idle.
constexpr uint32_t THANK_YOU_MS = 3000;

/// How long the cancellation message stays up before returning to Idle.
constexpr uint32_t ABANDONED_MS = 3000;

/// Minimum time on the fault screen, so an intermittent fault cannot strobe
/// the display by clearing and re-asserting.
constexpr uint32_t FAULT_MIN_MS = 5000;

// --- The panel button -------------------------------------------------------

/// How long the user button must be held to count as a hold rather than a
/// press. Long enough that nobody triggers an SD write by leaning on the panel,
/// short enough that three seconds of nothing happening does not read as a dead
/// button — which is why the screen appears the instant it qualifies.
constexpr uint32_t USER_BUTTON_HOLD_MS = 3000;

/// How long the two-button utility menu waits before giving up and returning
/// to Idle.
///
/// The most generous screen in the system, and deliberately: it is the only one
/// where somebody is expected to be doing something physical — finding the SD
/// card, emptying the coin box — between arriving at the screen and pressing a
/// button. Every other timeout assumes a customer who is already deciding.
///
/// It also takes the fridge out of service while it is up, so this is the
/// number to lower if that ever matters more than the convenience.
constexpr uint32_t UTILITY_MENU_MS = 30000;

/// How long "Write successful / failed — remove SD card" stays up.
///
/// Longer than the other confirmations because it is an INSTRUCTION, not just
/// feedback: somebody has to read it, reach over and pull the card out. Three
/// seconds is enough to read "Thank you"; it is not enough to act on.
constexpr uint32_t SD_RESULT_MS = 8000;

/// How long the tare result and its reading stay up.
///
/// Shorter than SD_RESULT_MS because nothing has to be done afterwards — the
/// number is there to be READ, as confirmation that the cell is now zeroed and
/// responding, not acted on.
constexpr uint32_t TARE_RESULT_MS = 5000;

/// How long the useless-button message stays up. Short — it is a joke, and the
/// fridge should not be out of service for it.
constexpr uint32_t USELESS_BUTTON_MS = 2000;

// --- The link to the Pi -----------------------------------------------------

/// How often to announce that this board is alive, as `EVT HB`.
///
/// Not cosmetic. The Pi judges the link on how recently ANY frame arrived, and
/// the board has nothing to say while the fridge is idle — so without this a
/// healthy link looks dead simply because nobody was buying anything.
constexpr uint32_t LINK_HEARTBEAT_MS = 10000;

/// How long without a single valid frame before the link is called unhealthy,
/// which takes the terminal out of service with FAULT_PI_UNREACHABLE.
///
/// MUST MATCH `LINK_TIMEOUT_S` IN `fridged/config.py`. Both ends judge each
/// other by this number, and if they disagree one will declare the link dead
/// while the other still thinks it is fine — which presents as a fridge that
/// faults for no reason the Pi's logs can explain.
///
/// Generous on purpose: the Pi has nothing to say while the fridge is idle, so
/// silence is normal. What this catches is the cable being pulled or `fridged`
/// dying. It also bounds how long a blocking operation may hold the superloop
/// before the link is judged dead — see the note on `sd_log::dump_to_card()`.
constexpr uint32_t LINK_TIMEOUT_MS = 30000;

// --- Sensors and reporting --------------------------------------------------

/// How often the RFID reader is asked whether a card is present. Fast enough
/// that a tap feels instant; slow enough that it is not most of the superloop.
constexpr uint32_t NFC_POLL_INTERVAL_MS = 250;

/// How long to wait before retrying a reader that failed to initialise, so a
/// missing or unwired MFRC522 costs one line in the log every few seconds
/// rather than flooding it.
constexpr uint32_t NFC_RETRY_INTERVAL_MS = 5000;

/// How often the DS18B20s are read. This is the resolution of the dashboard's
/// temperature graph, so RAISING it makes the graph coarser, not just quieter.
///
/// THIS IS THE CEILING ON DASHBOARD FRESHNESS, not Grafana's refresh setting.
/// A browser polling every second still sees a reading that the board only
/// produces every interval; turning Grafana up without turning this down just
/// re-queries the same rows.
///
/// Floor is around 2 s: a DS18B20 conversion takes 750 ms, and three sensors
/// sampled much faster than that starts costing real superloop time.
constexpr uint32_t TEMP_SAMPLE_INTERVAL_MS = 10000;

/// How often the board reports its own condition to the Pi — die temperature,
/// money-box mass, fault count.
///
/// Matched to the temperature sample rate so the dashboard gets one coherent
/// picture per interval rather than two that disagree by a few seconds. Change
/// them together, and change `SELF_METRIC_INTERVAL_S` in `fridged/config.py`
/// with them — that is the Pi recording ITS half of the same health row.
constexpr uint32_t HEALTH_INTERVAL_MS = 10000;

// --- Diagnostics ------------------------------------------------------------

/// How often the "still alive" line is printed to the serial console. Its real
/// job is negative evidence: a heartbeat that stops, or arrives late, means
/// something in the loop has blocked, and that is the failure this project most
/// needs to catch.
///
/// Distinct from LINK_HEARTBEAT_MS, which is a frame sent to the Pi. This one is
/// only ever read by a human with a terminal open.
constexpr uint32_t CONSOLE_HEARTBEAT_MS = 10000;

/// Longest single pass through the superloop that is considered acceptable.
/// LVGL wants servicing every few milliseconds, so anything approaching this is
/// already a problem worth naming.
constexpr uint32_t LOOP_TIME_WARN_MS = 50;

} // namespace timings
