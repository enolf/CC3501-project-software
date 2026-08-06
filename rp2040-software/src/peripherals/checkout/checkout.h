#pragma once

#include <stdint.h>

// The transaction state machine.
//
// Stage 2 defines only the vocabulary: the states and the timing constants.
// run_checkout(), which owns every transition between them, arrives in stage 5.
// See plan.md section 1 for the state diagram and section 4 for what each state
// puts on the screen.
//
// The flow in one line:
//   door opens -> customer takes drinks -> door closes -> the Pi recounts ->
//   the difference is the basket -> pay by cash or QR -> thank you -> idle.
//
// A card tap is the other way in, and it goes through Greeting first: the
// holder is named on screen and the machine then waits for the door exactly as
// Idle does. An unrecognised card lands on AccessDenied, which says so and
// times out. Neither state locks anything — there is no lock — so the door on
// its own still starts a transaction; the card just makes the terminal greet
// someone it knows.
//
// The door is what gates progress. Nothing advances toward payment until the
// door is shut, because that is the only moment the camera can see a stable,
// unobstructed shelf (dashboard.md section 4.2).

namespace checkout {

/// Every state the terminal can be in. Exactly one is current at any time.
enum class State : uint8_t {
    /// Waiting. Screen is black. Woken by the door opening or an approved card.
    Idle,

    /// An approved card was tapped. The holder is greeted by name and the
    /// machine waits for the door, which is the same thing Idle was doing —
    /// this state exists so that "somebody has identified themselves but has
    /// not opened the door yet" is visible on screen and in the log, rather
    /// than being indistinguishable from a customer already choosing.
    ///
    /// Deliberately does NOT ask the Pi to scan. The scan is requested when the
    /// door opens, exactly as it is from Idle, so both ways in produce the same
    /// sequence of requests and there is only one scan-timing behaviour to
    /// reason about.
    Greeting,

    /// A card was read that is not on the approved list. Says so for a few
    /// seconds, then returns to Idle. Nothing is prevented: there is no lock,
    /// so this is a message, not an enforcement point.
    AccessDenied,

    /// The customer is at the open fridge. The Pi has been asked to scan.
    /// Waiting for the door to close.
    Selecting,

    /// Door shut. Waiting for the Pi to settle and report what is left on the
    /// shelf. The basket is worked out here, once, by difference.
    Recount,

    /// Showing the basket and the total, with Cash and Online touch targets.
    PaymentSelect,

    /// Showing amount paid against amount owed, updating as coins land.
    PayCash,

    /// Asked the Pi for a Square payment link; waiting for the URL.
    PayOnlineLink,

    /// Showing the QR code; waiting for Square to confirm payment.
    PayOnlineQr,

    /// Paid. Brief confirmation, then back to Idle.
    ThankYou,

    /// Timed out with drinks taken and not paid for. The theft is recorded and
    /// the system returns to Idle — theft is logged, not prevented.
    Abandoned,

    /// Not fit to trade (the Pi is unreachable, the scale has failed). Shows a
    /// generic out-of-service screen with a code; details go to the log.
    Fault,

    /// The maintenance menu: tare the money box, or write the log to SD.
    /// Reached only from Idle, by holding the panel button.
    ///
    /// Both jobs are things a person does to the fridge rather than things a
    /// customer does with it, which is why they share one screen behind a
    /// deliberate gesture instead of appearing anywhere a customer will look.
    UtilityMenu,

    /// Showing the outcome of an SD log write, and telling somebody they can
    /// take the card out.
    ///
    /// There is no state for the write ITSELF. `sd_log::dump_to_card()` blocks
    /// the superloop from start to finish, so no state machine pass could
    /// observe one — the "Writing…" screen is drawn and forced out to the panel
    /// immediately before the call instead.
    SdResult,

    /// Showing the outcome of a money-box tare, with the reading it produced.
    ///
    /// The reading matters as much as the word "successful": a tare that
    /// worked leaves the cell reading approximately zero, so a number that is
    /// not approximately zero says the tare ran but the cell is not behaving.
    TareResult,

    /// Telling somebody the panel button does nothing. Reached only from Idle,
    /// by a short press.
    UselessButton,
};

/// How a transaction was paid for. Reported to the Pi so the dashboard can
/// split revenue by method.
enum class PaymentMethod : uint8_t {
    None,   ///< Not chosen yet, or the transaction was abandoned
    Cash,
    Online,
};

inline const char *payment_method_name(PaymentMethod method)
{
    switch (method) {
        case PaymentMethod::None:   return "none";
        case PaymentMethod::Cash:   return "cash";
        case PaymentMethod::Online: return "card";
    }
    return "unknown";
}

/// Why a transaction ended. Goes straight into the TXN_END frame.
enum class Outcome : uint8_t {
    Paid,       ///< Money confirmed
    Stolen,     ///< Timed out with drinks taken and not paid for
    Cancelled,  ///< Ended before anything was owed
};

inline const char *outcome_name(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Paid:      return "paid";
        case Outcome::Stolen:    return "stolen";
        case Outcome::Cancelled: return "cancelled";
    }
    return "unknown";
}

/// Name of a state, for logging. Inline so this header needs no source file
/// until the state machine itself exists.
inline const char *state_name(State state)
{
    switch (state) {
        case State::Idle:          return "Idle";
        case State::Greeting:      return "Greeting";
        case State::AccessDenied:  return "AccessDenied";
        case State::Selecting:     return "Selecting";
        case State::Recount:       return "Recount";
        case State::PaymentSelect: return "PaymentSelect";
        case State::PayCash:       return "PayCash";
        case State::PayOnlineLink: return "PayOnlineLink";
        case State::PayOnlineQr:   return "PayOnlineQr";
        case State::ThankYou:      return "ThankYou";
        case State::Abandoned:     return "Abandoned";
        case State::Fault:         return "Fault";
        case State::UtilityMenu:   return "UtilityMenu";
        case State::SdResult:      return "SdResult";
        case State::TareResult:    return "TareResult";
        case State::UselessButton: return "UselessButton";
    }
    return "Unknown";
}

// --- Timeouts ---
// Moved to `src/timings.h`, which now holds every interval that shapes how the
// system feels to use. They are referenced from checkout.cpp as
// `timings::SELECT_TIMEOUT_MS` and so on.
//
// They left this header because several of them have to agree with numbers that
// are nowhere near it — SQUARE_LINK_TIMEOUT_MS against the Pi's HTTP timeout,
// LINK_TIMEOUT_MS against `fridged/config.py` — and a constant whose correctness
// depends on another file is best kept next to the others in the same position,
// not filed under the one state machine that happens to read it.

// --- Fault codes ---
// Shown on the out-of-service screen. Deliberately terse: the customer only
// needs to know it is broken, and whoever fixes it reads the serial log.

constexpr uint16_t FAULT_NONE            = 0;
constexpr uint16_t FAULT_PI_TIMEOUT      = 1;   ///< Asked for a scan, no reply
constexpr uint16_t FAULT_PI_UNREACHABLE  = 2;   ///< Link reports itself unhealthy
constexpr uint16_t FAULT_SCALE           = 3;   ///< Coin scale not responding
constexpr uint16_t FAULT_MANUAL          = 99;  ///< Raised from the debug key

// --- Public interface ---

/// Reset to Idle and show the idle screen. Call once at startup, after the
/// display and the Pi link are up.
void init();

/// Run the state machine: drain the event queue, apply timeouts, drive the
/// screen. Call every pass of the superloop. Non-blocking.
///
/// This is the ONLY function in the system that changes state. Peripheral tasks
/// raise events; what those events mean is decided here and nowhere else.
void run_checkout();

/// The current state, for logging and diagnostics.
State current();

/// How many faults have been raised since boot. Reported in the HEALTH frame,
/// where a rising count is the signal worth alerting on.
uint32_t fault_count();

// --- Appearance ---

/// What the idle screen shows.
///
/// Set at BUILD TIME from CMake, alongside the other build switches:
///
///     cmake ..                    black idle screen (the default)
///     cmake .. -DIDLE_LOGO=ON     show the society logo
///
/// Black is the better default and the reason is not aesthetic: the terminal
/// spends nearly all its life on this screen, so black means no burn-in and
/// less power, and it is unmistakably "nothing in progress" rather than a
/// transaction that has got stuck.
///
/// Behind one constant so the choice never reaches the state machine — nothing
/// in checkout.cpp knows or cares what Idle looks like.
enum class IdleScreen : uint8_t {
    Black,
    Logo,
};

#ifndef IDLE_SHOW_LOGO
#define IDLE_SHOW_LOGO 0
#endif

// constexpr rather than leaving the #define exposed: the preprocessor has no
// concept of namespaces, so a bare macro would look scoped while being global.
constexpr IdleScreen IDLE_SCREEN_STYLE =
    IDLE_SHOW_LOGO ? IdleScreen::Logo : IdleScreen::Black;

} // namespace checkout
