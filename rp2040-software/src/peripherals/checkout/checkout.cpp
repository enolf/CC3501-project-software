// The transaction state machine. See checkout.h for the states and plan.md
// section 1 for the diagram.
//
// HOW TO READ THIS FILE
// --------------------
// Two functions do all the work:
//
//   handle_event()  what each event means in each state
//   check_timeouts()  what happens when a state is left alone too long
//
// and one function, enter(), performs every transition. Nothing else changes
// `state`, so every possible move is in one place and the machine can be
// checked against the diagram by reading down the page.
//
// EVERY STATE MUST BE ABLE TO LEAVE ON ITS OWN. A customer who walks away must
// never strand the terminal, so any state that waits for a person has a
// timeout. The only states without one are those that leave by themselves
// anyway (ThankYou, Abandoned) and Idle, which is where nothing is happening.

#include "peripherals/checkout/checkout.h"

#include <stdio.h>
#include <inttypes.h>

#include "pico/stdlib.h"

#include "sim_config.h"
#include "drivers/logging/logging.h"
#include "peripherals/access_control/access_control.h"
#include "peripherals/basket/basket.h"
#include "peripherals/catalogue/catalogue.h"
#include "peripherals/events/events.h"
#include "peripherals/pi_link/pi_link.h"
#include "peripherals/scale_task/scale_task.h"
#include "peripherals/sd_log/sd_log.h"
#include "peripherals/tft_display/tft_display.h"

namespace checkout {

namespace {

// --- The machine's own state ---

State    state = State::Idle;
uint32_t state_since_ms = 0;

/// The shelf as last reported. The difference between this and the next report
/// is what the customer took.
basket::Inventory previous_inventory;

/// What is being bought right now.
basket::Basket current_basket;
uint32_t owed_cents = 0;

/// Cash received this transaction. `paid_grams` is what actually decides
/// whether the customer has paid (see basket::check_mass); `paid_cents` is the
/// running tally, which is only ever shown to the customer.
uint32_t paid_cents = 0;
double   paid_grams = 0.0;

PaymentMethod method = PaymentMethod::None;
uint32_t transaction_id = 0;
uint16_t fault_code = FAULT_NONE;

/// Who tapped in, for the greeting screen. A pointer into the compile-time
/// table in access_control.cpp, never an owned string, so there is nothing to
/// copy and nothing to free. nullptr means "nobody has tapped", which is also
/// the greeting shown if a UID stops resolving to a name.
const char *greeted_name = nullptr;

char square_url[pi_link::URL_MAX_LEN] = {};

uint32_t now_ms()
{
    return to_ms_since_boot(get_absolute_time());
}

uint32_t elapsed_ms()
{
    return now_ms() - state_since_ms;
}

/// Restart the current state's timeout. Called on every coin and every touch,
/// so a customer who is slowly finding coins is never cut off mid-payment.
void restart_timeout()
{
    state_since_ms = now_ms();
}

bool is_payment_state(State s)
{
    return s == State::PaymentSelect || s == State::PayCash ||
           s == State::PayOnlineLink || s == State::PayOnlineQr;
}

/// Perform a transition. The ONLY place `state` is assigned.
void enter(State next)
{
    const State previous = state;
    state = next;
    state_since_ms = now_ms();

    logf(LogLevel::INFORMATION, "checkout: %s -> %s",
         state_name(previous), state_name(next));

    // --- Abandoning an online payment kills the checkout link ---
    //
    // Leaving an online payment state for anywhere other than the QR screen
    // means the customer is not going to pay that link: they pressed BACK to
    // use cash, they walked away and timed out, Square failed, or a fault was
    // raised. A link left live stays payable indefinitely, so somebody could
    // pay for a drink already written off — or pay twice, having also fed the
    // coin box.
    //
    // Putting it HERE, in the one function that performs every transition,
    // rather than at each of those four call sites is the whole reason enter()
    // is the only place `state` is assigned. Four separate cancels would be
    // four chances to forget one, and a fifth exit path added next year would
    // silently not cancel at all.
    //
    // Two transitions are excluded, and both matter:
    //   PayOnlineLink -> PayOnlineQr   the normal progression, not an exit
    //   PayOnlineQr   -> ThankYou      reached BECAUSE the link was paid
    const bool was_paying_online = (previous == State::PayOnlineLink ||
                                    previous == State::PayOnlineQr);
    const bool still_paying_online = (next == State::PayOnlineQr ||
                                      next == State::ThankYou);
    if (was_paying_online && !still_paying_online) {
        pi_link::request_square_cancel(transaction_id);
    }

    // Each state owns exactly one screen, so the display is driven from the
    // transition rather than scattered through the event handling.
    switch (next) {
        case State::Idle:          Display::show_idle();          break;
        case State::Greeting:      Display::show_greeting(greeted_name); break;
        case State::AccessDenied:  Display::show_access_denied(); break;
        case State::Selecting:     Display::show_selecting();     break;
        case State::Recount:       Display::show_recount();       break;
        case State::PaymentSelect:
            // Tell the coin classifier to forget where the mass was sitting, so
            // coins already in the box from earlier transactions drop out of the
            // arithmetic. Done HERE rather than on entering PayCash because it
            // takes about half a second of steady readings for the new baseline
            // to establish, and the customer is still deciding which button to
            // press. By the time they tap CASH it is ready.
            scale::begin_transaction();
            Display::show_payment_select(current_basket, owed_cents);
            break;
        case State::PayCash:       Display::show_cash_progress(paid_cents,
                                                               owed_cents);  break;
        case State::PayOnlineLink: Display::show_online_waiting(); break;
        case State::PayOnlineQr:   Display::show_qr(square_url, owed_cents); break;
        case State::ThankYou:      Display::show_thanks(paid_cents,
                                                        owed_cents);   break;
        case State::Abandoned:     Display::show_cancelled();      break;
        case State::Fault:         Display::show_fault(fault_code); break;
    }
}

/// Clear everything belonging to a transaction and go back to Idle.
void finish_transaction()
{
    current_basket = basket::Basket{};
    owed_cents = 0;
    paid_cents = 0;
    paid_grams = 0.0;
    method = PaymentMethod::None;
    transaction_id = 0;
    square_url[0] = '\0';
    greeted_name = nullptr;
    enter(State::Idle);
}

/// Render a UID as hex, for the log and the SD card.
void format_uid(char *out, size_t length, const uint8_t *uid, uint8_t uid_len)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < uid_len && i < events::EVENT_UID_MAX_LEN; i++) {
        const int written = snprintf(out + used, length - used, "%02X", uid[i]);
        if (written < 0 || (size_t)written >= length - used) {
            break;
        }
        used += (size_t)written;
    }
}

uint32_t faults_raised = 0;

void raise_fault(uint16_t code)
{
    fault_code = code;
    faults_raised++;
    logf(LogLevel::ERROR, "checkout: fault %u raised in %s", code, state_name(state));
    sd_log::write_linef("FAULT code=%u state=%s", code, state_name(state));
    enter(State::Fault);
}

/// Wake up: ask the Pi to start looking, and show the welcome screen.
void begin_selecting()
{
    transaction_id = now_ms();
    pi_link::request_scan();
    enter(State::Selecting);
}

/// An approved card was tapped: greet the holder and wait for the door.
void begin_greeting(const events::Event &event)
{
    // The event carries the UID and not the name, so the name is fetched from
    // the module that owns the policy rather than being copied through the
    // queue. That is the pattern events.h describes at length: payloads stay
    // small and identical in size, and the state machine asks the owning module
    // for anything bigger. It also means the approved list is read at the one
    // moment it is acted on, so a card revoked between the tap and here is
    // greeted anonymously rather than by a name captured earlier.
    greeted_name = access_lookup(event.card.uid, event.card.len);

    char uid_text[2 * events::EVENT_UID_MAX_LEN + 1];
    format_uid(uid_text, sizeof(uid_text), event.card.uid, event.card.len);

    if (greeted_name != nullptr) {
        logf(LogLevel::INFORMATION, "checkout: %s tapped in (%s)",
             greeted_name, uid_text);
    } else {
        // Approved by the reader, unknown to the lookup. In a bench build this
        // is usually the SIM_NFC key with an empty approved list; on hardware
        // it would mean the two disagree, which is worth seeing.
        logf(LogLevel::WARNING,
             "checkout: card %s approved but has no name in the list", uid_text);
    }

    // BLOCKS, up to a couple of hundred milliseconds on a card mid-erase (see
    // sd_log.h). Accepted here for the same reason it is accepted for TXN_END
    // and FAULT: who got in and who did not is exactly the history worth having
    // on the card, and a tap is nowhere near a time-critical path. Moving this
    // after enter() would not help — LVGL does not render until Display::run()
    // later in the same pass, so the screen changes after the write regardless.
    sd_log::write_linef("ACCESS granted uid=%s name=%s", uid_text,
                        greeted_name != nullptr ? greeted_name : "unknown");
    enter(State::Greeting);
}

/// A card that is not on the approved list. Say so, and change nothing else.
///
/// Nothing is prevented here, and that is not an oversight: the fridge has no
/// lock, so the door is still a perfectly good way in and a denied card must
/// not leave the terminal unusable. What this buys is an honest message to the
/// person holding the card, and a line in the log for whoever enrols them.
void deny_access(const events::Event &event)
{
    char uid_text[2 * events::EVENT_UID_MAX_LEN + 1];
    format_uid(uid_text, sizeof(uid_text), event.card.uid, event.card.len);

    logf(LogLevel::WARNING, "checkout: card %s refused - not on the list",
         uid_text);
    sd_log::write_linef("ACCESS denied uid=%s", uid_text);
    enter(State::AccessDenied);
}

/// The drinks were taken and not paid for. Record it and go back to Idle.
void abandon()
{
    logf(LogLevel::WARNING,
         "checkout: abandoned, owed %" PRIu32 "c paid %" PRIu32 "c - logged as stolen",
         owed_cents, paid_cents);

    pi_link::notify_transaction_end(Outcome::Stolen, owed_cents, paid_cents,
                                    transaction_id);
    sd_log::write_linef("TXN_END id=%" PRIu32 " outcome=stolen owed=%" PRIu32
                        " paid=%" PRIu32, transaction_id, owed_cents, paid_cents);
    enter(State::Abandoned);
}

/// Payment is complete.
void complete_payment()
{
    logf(LogLevel::INFORMATION,
         "checkout: paid by %s, owed %" PRIu32 "c paid %" PRIu32 "c",
         payment_method_name(method), owed_cents, paid_cents);

    pi_link::notify_sale(current_basket, owed_cents, method, transaction_id);
    pi_link::notify_transaction_end(Outcome::Paid, owed_cents, paid_cents,
                                    transaction_id);
    sd_log::write_linef("TXN_END id=%" PRIu32 " outcome=paid method=%s owed=%" PRIu32
                        " paid=%" PRIu32, transaction_id,
                        payment_method_name(method), owed_cents, paid_cents);
    enter(State::ThankYou);
}

/// Work out what changed on the shelf and decide whether there is anything to
/// charge for. Called once, on the report that follows the door closing.
void apply_inventory()
{
    basket::Inventory current;
    if (!pi_link::poll_inventory(current)) {
        // The doorbell rang with nothing behind it. Not fatal, but it means the
        // link and the state machine disagree, which is worth seeing.
        log(LogLevel::WARNING, "checkout: inventory event with no data");
        return;
    }

    basket::Basket taken;
    const basket::Change change = basket::diff(previous_inventory, current, taken);

    // The new shelf becomes the baseline whatever happened. Skipping this after
    // a restock would make the next scan look like an enormous purchase.
    previous_inventory = current;

    switch (change) {
        case basket::Change::None:
            log(LogLevel::INFORMATION, "checkout: nothing taken");
            enter(State::Idle);
            return;

        case basket::Change::Restocked:
            log(LogLevel::INFORMATION, "checkout: restocked, nobody is charged");
            sd_log::write_line("RESTOCK");
            enter(State::Idle);
            return;

        case basket::Change::Mixed:
            // A drink was put back and another taken. Normal customer
            // behaviour, and the basket already holds only what left.
            log(LogLevel::INFORMATION, "checkout: a drink was swapped");
            break;

        case basket::Change::ItemsTaken:
            break;
    }

    current_basket = taken;
    owed_cents = basket::total_cents(taken);

    if (owed_cents == 0) {
        // Can only happen if a drink is priced at zero. Nothing to collect.
        log(LogLevel::WARNING, "checkout: items taken but nothing owed");
        enter(State::Idle);
        return;
    }

    logf(LogLevel::INFORMATION, "checkout: %u item(s), owed %" PRIu32 "c",
         basket::item_count(taken), owed_cents);
    enter(State::PaymentSelect);
}

/// Fold a coin into the running total and decide whether that settles the bill.
void apply_coin(const events::Event &event)
{
    paid_cents += (uint32_t)(event.coin.cents > 0 ? event.coin.cents : 0);
    paid_grams += (double)event.coin.delta_grams;

    pi_link::notify_coin(event.coin.cents, (double)event.coin.delta_grams, true);

    // A coin is activity, so the customer gets the full timeout again.
    restart_timeout();
    Display::show_cash_progress(paid_cents, owed_cents);

    // The MASS decides, not the tally. See the long comment in basket.h: a
    // missed coin corrupts the tally for the rest of the transaction, but the
    // total mass in the box is the same however the coins got there.
    const basket::MassVerdict verdict = basket::check_mass(owed_cents, paid_grams);
    if (verdict == basket::MassVerdict::Overpaid) {
        logf(LogLevel::INFORMATION,
             "checkout: overpaid (%.2f g in the box) - no change given", paid_grams);
    }
    if (verdict != basket::MassVerdict::Insufficient) {
        complete_payment();
    }
}

// -------------------------------------------------------------------------
// What each event means in each state
// -------------------------------------------------------------------------
void handle_event(const events::Event &event)
{
    // --- Events handled the same way everywhere ---

    if (event.kind == events::Kind::Fault) {
        raise_fault(event.fault.code);
        return;
    }

    // A late payment belongs to a transaction that is already over, so it is
    // handled here rather than in any one state — by the time it arrives the
    // machine is usually back in Idle, which is why the event carries its own
    // transaction id instead of relying on the current one.
    //
    // Nothing can be done automatically: the drinks have gone, the transaction
    // is closed, and this firmware has no authority to move money. The only
    // useful action is to make it FINDABLE — serial for whoever is watching at
    // the time, SD card for whoever is not. Decision D17: the refund is a human
    // job, and this line is the evidence that one is owed.
    if (event.kind == events::Kind::SquareLatePaid) {
        logf(LogLevel::ERROR,
             "checkout: REFUND OWED - Square took %" PRIu32 "c for txn %" PRIu32
             " after the link was cancelled",
             event.payment.cents, event.payment.txn_id);
        sd_log::write_linef("REFUND_OWED id=%" PRIu32 " cents=%" PRIu32,
                            event.payment.txn_id, event.payment.cents);
        return;
    }

    // Door movements are always reported to the Pi, whatever the state, because
    // the dashboard wants every one of them.
    if (event.kind == events::Kind::DoorOpened ||
        event.kind == events::Kind::DoorClosed) {
        pi_link::notify_door(event.kind == events::Kind::DoorOpened);
    }

    if (event.kind == events::Kind::CardApproved ||
        event.kind == events::Kind::CardDenied) {
        pi_link::notify_rfid(event.card.uid, event.card.len,
                             event.kind == events::Kind::CardApproved);
    }

    if (event.kind == events::Kind::CoinRejected) {
        pi_link::notify_coin(0, (double)event.coin.delta_grams, false);
        log(LogLevel::WARNING, "checkout: unrecognised object in the coin box");
        return;
    }

    // --- Per-state handling ---

    switch (state) {
        case State::Idle:
            // Two ways in, and they are no longer the same way.
            //
            // The door opening means a customer is already at the shelf, so
            // the transaction starts immediately. A card tap means somebody has
            // identified themselves but has not opened anything yet, so it goes
            // to Greeting and waits for the door — which is what Idle was doing
            // anyway, only now with their name on the screen.
            if (event.kind == events::Kind::DoorOpened) {
                begin_selecting();
            } else if (event.kind == events::Kind::CardApproved) {
                begin_greeting(event);
            } else if (event.kind == events::Kind::CardDenied) {
                deny_access(event);
            }
            break;

        case State::Greeting:
            // The greeting is a waiting room, so it accepts exactly what Idle
            // accepts. A second approved card re-greets whoever tapped last and
            // restarts the timeout, which is what happens when two people
            // arrive together.
            if (event.kind == events::Kind::DoorOpened) {
                begin_selecting();
            } else if (event.kind == events::Kind::CardApproved) {
                begin_greeting(event);
            }
            // CardDenied is deliberately ignored here. Somebody approved is
            // already standing at the fridge; wiping their greeting because a
            // stranger waved an unknown card at the reader would punish the
            // wrong person. The refusal is already logged and sent to the Pi by
            // the common handling above.
            break;

        case State::AccessDenied:
            // Leaves on its own after DENIED_MS, but must not ignore the world
            // meanwhile: the door still works, and a second tap of a good card
            // should be believed immediately rather than after the timer.
            if (event.kind == events::Kind::DoorOpened) {
                begin_selecting();
            } else if (event.kind == events::Kind::CardApproved) {
                begin_greeting(event);
            } else if (event.kind == events::Kind::CardDenied) {
                // Re-entering redraws the screen and restarts the timer, so
                // somebody trying a second card gets an answer to that card
                // rather than a screen that vanishes mid-tap.
                deny_access(event);
            }
            break;

        case State::Selecting:
            // Progress waits for the door to shut. That is the only moment the
            // camera has a stable, unobstructed view of the shelf, which is
            // what makes the whole vision approach workable.
            if (event.kind == events::Kind::DoorClosed) {
                pi_link::request_scan();
                enter(State::Recount);
            }
            break;

        case State::Recount:
            if (event.kind == events::Kind::InventoryUpdated) {
                apply_inventory();
            }
            break;

        case State::PaymentSelect:
            if (event.kind == events::Kind::TouchCash) {
                if (!scale::is_ready()) {
                    // Without the scale there is no way to know money arrived.
                    // In a build with the coin-simulation keys compiled in this
                    // is only a warning, so the flow can still be walked at a
                    // desk; in a build without them it is a genuine fault.
#if SIM_COINS
                    log(LogLevel::WARNING,
                        "checkout: no scale - cash accepted from debug keys only");
#else
                    raise_fault(FAULT_SCALE);
                    break;
#endif
                }
                method = PaymentMethod::Cash;
                paid_cents = 0;
                paid_grams = 0.0;
                pi_link::notify_sale(current_basket, owed_cents, method,
                                     transaction_id);
                enter(State::PayCash);
            } else if (event.kind == events::Kind::TouchOnline) {
                method = PaymentMethod::Online;
                pi_link::request_square_link(owed_cents, transaction_id,
                                             current_basket);
                enter(State::PayOnlineLink);
            }
            break;

        case State::PayCash:
            if (event.kind == events::Kind::CoinAccepted) {
                apply_coin(event);
            }
            break;

        case State::PayOnlineLink:
            if (event.kind == events::Kind::SquareUrlReady) {
                if (pi_link::poll_square_url(square_url, sizeof(square_url))) {
                    logf(LogLevel::INFORMATION, "checkout: payment link %s",
                         square_url);
                    enter(State::PayOnlineQr);
                }
            } else if (event.kind == events::Kind::SquareError) {
                // Not a fault: cash is still available, so fall back to the
                // choice rather than taking the terminal out of service.
                (void)pi_link::poll_square_error();
                log(LogLevel::WARNING,
                    "checkout: no payment link, falling back to the payment choice");
                enter(State::PaymentSelect);
            }
            break;

        case State::PayOnlineQr:
            if (event.kind == events::Kind::SquarePaid) {
                (void)pi_link::poll_square_paid();
                paid_cents = owed_cents;   // Square only confirms the exact amount
                complete_payment();
            } else if (event.kind == events::Kind::TouchBack) {
                log(LogLevel::INFORMATION, "checkout: customer switched to cash");
                restart_timeout();
                enter(State::PaymentSelect);
            }
            break;

        case State::ThankYou:
        case State::Abandoned:
            // Leaving on a timer. Nothing a customer does should cut these
            // short or extend them.
            break;

        case State::Fault:
            break;
    }

    // Any touch counts as the customer still being present.
    if (is_payment_state(state) &&
        (event.kind == events::Kind::TouchCash ||
         event.kind == events::Kind::TouchOnline ||
         event.kind == events::Kind::TouchBack)) {
        restart_timeout();
    }
}

// -------------------------------------------------------------------------
// What happens when a state is left alone too long
// -------------------------------------------------------------------------
void check_timeouts()
{
    switch (state) {
        case State::Idle:
            break;   // nothing in progress, nothing to time out

        case State::Greeting:
            // Tapped in and then walked away, or tapped out of curiosity.
            // Nothing was started, so there is nothing to abandon or record —
            // this is just the screen going back to sleep.
            if (elapsed_ms() >= GREETING_TIMEOUT_MS) {
                logf(LogLevel::INFORMATION,
                     "checkout: %s tapped in but never opened the door",
                     greeted_name != nullptr ? greeted_name : "somebody");
                greeted_name = nullptr;
                enter(State::Idle);
            }
            break;

        case State::AccessDenied:
            if (elapsed_ms() >= DENIED_MS) {
                enter(State::Idle);
            }
            break;

        case State::Selecting:
            // Reached by someone leaving the fridge door standing open.
            if (elapsed_ms() >= SELECT_TIMEOUT_MS) {
                log(LogLevel::INFORMATION, "checkout: nobody took anything");
                enter(State::Idle);
            }
            break;

        case State::Recount:
            // The Pi is not answering. This one IS a fault: with no inventory
            // there is no way to know what to charge for.
            if (elapsed_ms() >= RECOUNT_TIMEOUT_MS) {
                raise_fault(FAULT_PI_TIMEOUT);
            }
            break;

        case State::PayOnlineLink:
            // A payment link that never arrives falls back to the choice, so
            // cash still works. Checked before the payment timeout because it
            // is the shorter of the two.
            if (elapsed_ms() >= SQUARE_LINK_TIMEOUT_MS) {
                log(LogLevel::WARNING, "checkout: payment link timed out");
                enter(State::PaymentSelect);
            }
            break;

        case State::PaymentSelect:
        case State::PayCash:
        case State::PayOnlineQr:
            if (elapsed_ms() >= PAYMENT_TIMEOUT_MS) {
                abandon();
            }
            break;

        case State::ThankYou:
            if (elapsed_ms() >= THANK_YOU_MS) {
                finish_transaction();
            }
            break;

        case State::Abandoned:
            if (elapsed_ms() >= ABANDONED_MS) {
                finish_transaction();
            }
            break;

        case State::Fault:
            // Held for a minimum time so an intermittent fault cannot strobe
            // the screen, then released once the cause has gone. The link being
            // healthy is the only condition currently checked; a fault raised
            // from the debug key therefore clears on its own after FAULT_MIN_MS.
            if (elapsed_ms() >= FAULT_MIN_MS && pi_link::is_healthy()) {
                log(LogLevel::INFORMATION, "checkout: fault cleared");
                fault_code = FAULT_NONE;
                finish_transaction();
            }
            break;
    }
}

} // namespace

void init()
{
    state = State::Idle;
    state_since_ms = now_ms();
    previous_inventory = basket::Inventory{};
    current_basket = basket::Basket{};
    owed_cents = 0;
    paid_cents = 0;
    paid_grams = 0.0;
    method = PaymentMethod::None;
    fault_code = FAULT_NONE;
    greeted_name = nullptr;

    // previous_inventory starts empty, so the very first scan reports every
    // drink as newly arrived and is classified as a restock. That is the
    // correct behaviour, not a special case: the first look at the shelf
    // establishes the baseline and charges nobody.

    Display::show_idle();
    log(LogLevel::INFORMATION, "checkout: ready, in Idle");
}

void run_checkout()
{
    events::Event event;
    while (events::pop(event)) {
        handle_event(event);
    }

    // The Pi going away takes the terminal out of service, because without it
    // there is no way to know what a customer took. Checked here rather than
    // raised as an event so it cannot be missed if the queue is busy.
    //
    // Deliberately NOT checked while already in Fault: the recovery test in
    // check_timeouts() is what decides when to come back, and re-raising the
    // fault every pass would reset its minimum display time forever.
    if (state != State::Fault && !pi_link::is_healthy()) {
        raise_fault(FAULT_PI_UNREACHABLE);
        return;
    }

    check_timeouts();
}

uint32_t fault_count()
{
    return faults_raised;
}

State current()
{
    return state;
}

} // namespace checkout
