// The transaction state machine. See checkout.h for the states, and
// documentation.md section 1 for the transaction flow as a whole.
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

#include "timings.h"

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

/// The shelf as it was when THIS customer arrived, captured from the scan
/// requested when the door first opened.
///
/// FIXED FOR THE WHOLE TRANSACTION. Every recount is measured against this one
/// snapshot, so the basket is always an absolute statement of what has left the
/// fridge since the customer walked up — never a running total of per-door-cycle
/// differences.
///
/// THAT DISTINCTION IS THE WHOLE FEATURE. The previous version re-baselined
/// after every recount, which made the basket differential, and three ordinary
/// things a customer does all came out wrong:
///
///   put a drink back      counts went UP against the new baseline, so it read
///                         as a restock and the customer was still charged
///   swap one for another  the returned drink vanished from the basket while
///                         the new one was added, so they were charged for both
///   change their mind     no way back to Idle; the sale stood
///
/// Against a fixed baseline all three are the same arithmetic as the first
/// recount, and none of them needs a special case. It is the same argument the
/// camera makes for reporting absolute counts rather than differences
/// (fridged/camera.py): a reading that is absolute cannot accumulate error, and
/// a reading that is differential eventually will.
basket::Inventory transaction_baseline;

/// Whether `transaction_baseline` holds a real reading yet. False between
/// transactions and until the door-open scan comes back.
bool has_baseline = false;

/// Whether the Pi has been told about this sale (`EVT TXN_START`). Decides
/// whether a transaction that ends up empty needs a matching `TXN_END`, so the
/// dashboard is not left with a sale that never concludes.
bool sale_announced = false;

/// What is being bought right now.
basket::Basket current_basket;
uint32_t owed_cents = 0;

/// Cash received this transaction. `paid_grams` is what actually decides
/// whether the customer has paid (see basket::check_mass); `paid_cents` is the
/// running tally, which is only ever shown to the customer.
uint32_t paid_cents = 0;

/// Outcome of the last SD log dump, for the screen that reports it.
bool sd_write_ok = false;
unsigned long sd_write_lines = 0;

/// Outcome of the last money-box tare, likewise.
bool   tare_ok = false;
double tare_grams = 0.0;
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
        case State::RefundOwed:    Display::show_refund_owed(paid_cents); break;
        case State::Fault:         Display::show_fault(fault_code); break;
        case State::UtilityMenu:   Display::show_utility_menu();   break;
        case State::SdResult:      Display::show_sd_result(sd_write_ok,
                                                           sd_write_lines); break;
        case State::TareResult:    Display::show_tare_result(tare_ok,
                                                             tare_grams); break;
        case State::UselessButton: Display::show_useless_button(); break;
    }
}

/// Clear everything belonging to a transaction and go back to Idle.
///
/// `transaction_id = 0` is load-bearing, not just tidiness: a non-zero id is
/// what "a transaction is in progress" MEANS everywhere else in this file, and
/// it is how begin_selecting() tells a customer arriving from Idle apart from
/// one who has reopened the door mid-sale. Any path back to Idle must come
/// through here, or the next customer inherits this one's transaction.
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

    // Dropped rather than carried over, so the next customer's baseline is a
    // fresh look at the shelf. That is what makes a restock between two
    // transactions invisible to the arithmetic instead of being charged to
    // whoever opens the door next.
    transaction_baseline = basket::Inventory{};
    has_baseline = false;
    sale_announced = false;

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
///
/// Reached only from a state where nothing is in progress (Idle, Greeting,
/// AccessDenied, UtilityMenu). A door opening DURING a transaction goes to
/// reopen_for_changes() instead, which is a different thing entirely — it must
/// not mint a new transaction id and must not disturb the baseline.
void begin_selecting()
{
    transaction_id = now_ms();
    has_baseline = false;
    sale_announced = false;

    // The reply to THIS scan becomes the baseline. It used to be requested and
    // then thrown away — Selecting ignored InventoryUpdated — which is why the
    // baseline had to come from the previous customer's recount, and why the
    // first purchase after boot was free.
    pi_link::request_scan();
    enter(State::Selecting);
}

/// The customer opened the door again in the middle of a transaction: to put a
/// drink back, to swap one, or to take another.
///
/// Keeps the transaction id AND the baseline, so the recount that follows
/// re-derives the basket from scratch rather than layering another difference
/// on top of the last one.
///
/// No scan is requested here. The baseline already exists and a second reading
/// of a shelf the customer is currently reaching into would be discarded
/// anyway; the recount on the way out is the one that counts.
///
/// Note what enter() does on the way out of an online payment state: it fires
/// `CMD SQUARE_CANCEL`. That is exactly right and comes for free — the basket
/// is about to change, so a live link for the old amount must not survive.
void reopen_for_changes()
{
    logf(LogLevel::INFORMATION,
         "checkout: door reopened during %s - the basket will be recounted",
         state_name(state));
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

/// Dump the buffered log to whatever card is in the slot. Idle only.
///
/// WHY THIS BLOCKS, AND WHY THAT IS ACCEPTABLE HERE
/// -----------------------------------------------
/// `dump_to_card()` mounts, writes every buffered line and unmounts, and holds
/// the superloop for the whole time — hundreds of milliseconds, seconds on a
/// slow card. Nothing else in this file is allowed to do that.
///
/// It is allowed here because of WHERE it is called from. In Idle there is no
/// transaction, no coin settling window, no payment timeout and no customer
/// waiting on a screen. The one thing that does suffer is the Pi link: no
/// heartbeat goes out while this runs, and at `LINK_TIMEOUT_MS` of 30 s there
/// is ample margin for a write that takes even a few seconds.
///
/// This is the same reasoning `sd_log.h` records for confining writes to Idle,
/// and it is the reason the panel button does nothing in any other state —
/// which is also what was asked for, so the constraint and the requirement
/// happen to agree.
/// Re-zero the money box. UtilityMenu only, and for the same reason as the SD
/// write: `retare()` blocks for about a second while it averages samples.
///
/// WHY THIS EXISTS AS A BUTTON
/// ---------------------------
/// The startup tare is the only other one, so a board rebooted with coins in
/// the box treats their mass as zero and every reading afterwards is short by
/// it — silently, and for as long as the board stays up. Emptying the box and
/// pressing this is the fix, and it needs no serial terminal and no reflash.
void tare_money_box()
{
    // Drawn and flushed before the blocking call, exactly as the SD write does.
    Display::show_taring();

    tare_ok = scale::retare();
    tare_grams = scale::box_grams();

    logf(LogLevel::INFORMATION, "checkout: money box tare %s, reads %.3f g",
         tare_ok ? "succeeded" : "FAILED", tare_grams);
    sd_log::write_linef("TARE %s grams=%.3f", tare_ok ? "ok" : "failed",
                        tare_grams);

    enter(State::TareResult);
}

void write_log_to_card()
{
    // Drawn and flushed BEFORE the write starts. Queued behind it, this screen
    // would appear only once the write had finished, announcing something that
    // was no longer happening.
    Display::show_sd_writing();

    sd_write_ok = sd_log::dump_to_card();
    sd_write_lines = sd_log::buffered_lines();

    logf(LogLevel::INFORMATION, "checkout: SD log write %s (%lu lines)",
         sd_write_ok ? "succeeded" : "FAILED", sd_write_lines);

    enter(State::SdResult);
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

/// The basket came out empty: either nothing was ever taken, or everything that
/// was taken has since been put back.
///
/// Split out because there are two very different ways to arrive here and only
/// one of them is uneventful.
void settle_empty_basket()
{
    current_basket = basket::Basket{};
    owed_cents = 0;

    // Coins are already in the box and there is nothing left to sell. The box
    // is one-way — there is no hopper and no way to give change — so this
    // cannot be resolved by the machine, only reported.
    //
    // Handled exactly like a Square late payment (decision D17): say so on
    // screen, and write the amount to serial AND the SD card so the two
    // accounts can be reconciled by whoever empties the box.
    if (paid_cents > 0 || paid_grams > 0.0) {
        logf(LogLevel::ERROR,
             "checkout: REFUND OWED - every drink was returned but %" PRIu32
             "c (%.2f g) is in the box for txn %" PRIu32,
             paid_cents, paid_grams, transaction_id);
        sd_log::write_linef("REFUND_OWED id=%" PRIu32 " cents=%" PRIu32
                            " grams=%.2f", transaction_id, paid_cents,
                            paid_grams);
        pi_link::notify_transaction_end(Outcome::Cancelled, 0, paid_cents,
                                        transaction_id);
        enter(State::RefundOwed);
        return;
    }

    // The Pi has a TXN_START for this id and would otherwise wait forever for
    // it to conclude. Only sent if the sale was actually announced — a customer
    // who never got as far as choosing a payment method was never mentioned.
    if (sale_announced) {
        pi_link::notify_transaction_end(Outcome::Cancelled, 0, 0, transaction_id);
        sd_log::write_linef("TXN_END id=%" PRIu32 " outcome=cancelled",
                            transaction_id);
    }

    log(LogLevel::INFORMATION, "checkout: nothing taken, nobody is charged");
    finish_transaction();
}

/// A scan came back. What it means depends entirely on which one it was.
///
/// Called for EVERY inventory report in every state, so a reply that arrives
/// somewhere unexpected is drained rather than left waiting — an undrained
/// report would be collected by the next customer's Selecting and used as their
/// baseline, which is a stale shelf reading charged to the wrong person.
void apply_inventory()
{
    basket::Inventory current;
    if (!pi_link::poll_inventory(current)) {
        // The doorbell rang with nothing behind it. Not fatal, but it means the
        // link and the state machine disagree, which is worth seeing.
        log(LogLevel::WARNING, "checkout: inventory event with no data");
        return;
    }

    // --- The baseline scan, requested when the door opened ---
    if (!has_baseline) {
        transaction_baseline = current;
        has_baseline = true;
        log(LogLevel::INFORMATION, "checkout: shelf baseline captured");

        // Arriving in Recount means the door shut before the baseline came
        // back — a quick open and close, or a slow Pi. Nothing is lost and
        // nothing needs a special case: entering Recount ALWAYS requests a
        // second scan, so the recount is already in flight and this reply is
        // simply the baseline arriving late. Stay put and wait for it.
        //
        // If it never comes, RECOUNT_TIMEOUT_MS raises FAULT_PI_TIMEOUT, which
        // is the honest answer — with one reading there is no way to know what
        // was taken, and guessing would either charge for nothing or give
        // drinks away.
        return;
    }

    if (state != State::Recount) {
        // A reply we no longer have any use for: the scan requested at the
        // door opening, arriving after the customer has already reopened the
        // door, or a stale report landing outside a transaction. Drained above,
        // discarded here.
        logf(LogLevel::INFORMATION,
             "checkout: inventory report arrived in %s, nothing to measure it "
             "against - discarded", state_name(state));
        return;
    }

    // --- The recount, after the door shut ---
    basket::Basket taken;
    const basket::Change change = basket::diff(transaction_baseline, current,
                                               taken);

    // THE BASELINE IS DELIBERATELY NOT MOVED. See the note on its declaration:
    // holding it still is what lets the customer open the door again and put a
    // drink back, and what makes this recount say what has left the fridge
    // rather than what changed since the last time we looked.

    if (change == basket::Change::Restocked) {
        // Counts only went up against the shelf as it was when the door
        // opened. Somebody refilled the fridge rather than buying from it.
        log(LogLevel::INFORMATION, "checkout: restocked, nobody is charged");
        sd_log::write_line("RESTOCK");
    } else if (change == basket::Change::Mixed) {
        // One drink back, another taken. Normal customer behaviour, and the
        // basket already holds only what left.
        log(LogLevel::INFORMATION, "checkout: a drink was swapped");
    }

    if (basket::is_empty(taken)) {
        settle_empty_basket();
        return;
    }

    current_basket = taken;
    owed_cents = basket::total_cents(taken);

    if (owed_cents == 0) {
        // Can only happen if a drink is priced at zero. Nothing to collect.
        log(LogLevel::WARNING, "checkout: items taken but nothing owed");
        settle_empty_basket();
        return;
    }

    logf(LogLevel::INFORMATION, "checkout: %u item(s), owed %" PRIu32 "c",
         basket::item_count(taken), owed_cents);

    // --- Where the customer goes next ---
    //
    // Straight back to the cash screen if coins are already committed, and NOT
    // through PaymentSelect. Entering PaymentSelect calls
    // scale::begin_transaction(), which re-zeroes the coin baseline — so
    // routing a customer with money in the box through it would erase
    // everything they had paid, silently, at the exact moment they were owed
    // the most care.
    if (method == PaymentMethod::Cash && (paid_cents > 0 || paid_grams > 0.0)) {
        // Re-announced with the new basket. `upsert_txn` and `set_txn_items` on
        // the Pi are both replace-not-append precisely so a second TXN_START
        // for one id corrects the first rather than doubling it.
        pi_link::notify_sale(current_basket, owed_cents, method, transaction_id);
        sale_announced = true;

        // The swap may have made the drinks cheaper than what is already in the
        // box, in which case they have finished paying without touching
        // anything. Asking the mass gate is the same question PayCash asks on
        // every coin, so there is one definition of "has this been paid for".
        if (basket::mass_gate_satisfied(owed_cents, paid_grams)) {
            log(LogLevel::INFORMATION,
                "checkout: the new basket is already covered by the coins in "
                "the box");
            complete_payment();
        } else {
            enter(State::PayCash);
        }
        return;
    }

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

    // Inventory reports are handled in ONE place rather than per state, and
    // handled in EVERY state rather than only where they are wanted.
    //
    // The reason is that poll_inventory() is a one-shot latch: a report nobody
    // collects stays waiting. Left in the queue by a state that did not care,
    // it would be picked up by the next customer's Selecting and taken as their
    // baseline — a shelf reading from minutes ago, charged to the wrong person.
    // apply_inventory() decides from `state` what the report means, including
    // deciding it means nothing.
    if (event.kind == events::Kind::InventoryUpdated) {
        apply_inventory();
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
            } else if (event.kind == events::Kind::UserButtonHeld) {
                enter(State::UtilityMenu);
            } else if (event.kind == events::Kind::UserButtonPressed) {
                enter(State::UselessButton);
            }
            break;

        case State::UtilityMenu:
            // The two targets are the same ones the payment screen uses, so
            // they arrive as TouchCash and TouchOnline. Only this state reads
            // them this way, and only Idle can reach it, so there is no chance
            // of a customer's payment tap landing here.
            if (event.kind == events::Kind::TouchCash) {
                tare_money_box();
            } else if (event.kind == events::Kind::TouchOnline) {
                write_log_to_card();
            } else if (event.kind == events::Kind::TouchBack) {
                enter(State::Idle);
            } else if (event.kind == events::Kind::DoorOpened) {
                // A customer arriving outranks maintenance. Nobody should have
                // to wait out a menu somebody else left open.
                begin_selecting();
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
            // Leaves on its own after timings::DENIED_MS, but must not ignore the world
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
            // Reopened while the camera was still counting. The reply that is
            // already in flight will be discarded, because a baseline exists
            // and the state will no longer be Recount when it lands.
            if (event.kind == events::Kind::DoorOpened) {
                reopen_for_changes();
            }
            break;

        case State::PaymentSelect:
            // Changed their mind before choosing how to pay. Nothing is
            // committed, so this is the cheapest case: back to Selecting, and
            // whatever the recount says is simply the new basket.
            if (event.kind == events::Kind::DoorOpened) {
                reopen_for_changes();
            } else if (event.kind == events::Kind::TouchCash) {
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
                sale_announced = true;
                enter(State::PayCash);
            } else if (event.kind == events::Kind::TouchOnline) {
                method = PaymentMethod::Online;
                pi_link::request_square_link(owed_cents, transaction_id,
                                             current_basket);
                enter(State::PayOnlineLink);
            }
            break;

        case State::PayCash:
            // Reopening with coins already in the box is allowed, and it is the
            // case that needs the most care. The recount routes back here
            // rather than through PaymentSelect so the coin baseline is never
            // re-zeroed under a customer who has already paid something; see
            // apply_inventory().
            if (event.kind == events::Kind::DoorOpened) {
                reopen_for_changes();
            } else if (event.kind == events::Kind::CoinAccepted) {
                apply_coin(event);
            }
            break;

        case State::PayOnlineLink:
            // enter() fires CMD SQUARE_CANCEL on the way out, so a link for the
            // old amount cannot outlive the basket it was created for.
            if (event.kind == events::Kind::DoorOpened) {
                reopen_for_changes();
            } else if (event.kind == events::Kind::SquareUrlReady) {
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
            // Same as PayOnlineLink: the QR on screen is for an amount that is
            // about to be recalculated, so the link is cancelled by enter().
            if (event.kind == events::Kind::DoorOpened) {
                reopen_for_changes();
            } else if (event.kind == events::Kind::SquarePaid) {
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
        case State::RefundOwed:
        case State::SdResult:
        case State::TareResult:
        case State::UselessButton:
            // Leaving on a timer. Nothing a customer does should cut these
            // short or extend them.
            //
            // RefundOwed in particular must NOT accept a door opening. Somebody
            // who is owed money and opens the fridge again is starting a fresh
            // visit, and the refund message has to finish being read first —
            // reopening from here would swallow it, and with it the only notice
            // the customer ever gets.
            //
            // The two button states are here rather than anywhere else because
            // that is the whole requirement: the panel button does something
            // ONLY from Idle. Listing them explicitly, and leaving -Wswitch on,
            // means a state added later cannot quietly inherit "ignores the
            // button" by falling through a default.
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
            if (elapsed_ms() >= timings::GREETING_TIMEOUT_MS) {
                logf(LogLevel::INFORMATION,
                     "checkout: %s tapped in but never opened the door",
                     greeted_name != nullptr ? greeted_name : "somebody");
                greeted_name = nullptr;
                enter(State::Idle);
            }
            break;

        case State::AccessDenied:
            if (elapsed_ms() >= timings::DENIED_MS) {
                enter(State::Idle);
            }
            break;

        case State::Selecting:
            // Reached by someone leaving the fridge door standing open.
            if (elapsed_ms() >= timings::SELECT_TIMEOUT_MS) {
                if (owed_cents > 0 || sale_announced) {
                    // They reopened the door mid-transaction and walked off
                    // with it hanging open. Drinks are out and unpaid, and that
                    // is a theft — going quietly to Idle here would erase a
                    // sale the machine had already worked out and announced.
                    log(LogLevel::WARNING,
                        "checkout: door left open with an unpaid basket");
                    abandon();
                } else {
                    // Nothing was ever owed, so there is nothing to record.
                    // finish_transaction() rather than enter(Idle): the
                    // transaction id must be cleared, or the next customer
                    // inherits it and their door-open is mistaken for a reopen.
                    log(LogLevel::INFORMATION, "checkout: nobody took anything");
                    finish_transaction();
                }
            }
            break;

        case State::Recount:
            // The Pi is not answering. This one IS a fault: with no inventory
            // there is no way to know what to charge for.
            if (elapsed_ms() >= timings::RECOUNT_TIMEOUT_MS) {
                raise_fault(FAULT_PI_TIMEOUT);
            }
            break;

        case State::PayOnlineLink:
            // A payment link that never arrives falls back to the choice, so
            // cash still works. Checked before the payment timeout because it
            // is the shorter of the two.
            if (elapsed_ms() >= timings::SQUARE_LINK_TIMEOUT_MS) {
                log(LogLevel::WARNING, "checkout: payment link timed out");
                enter(State::PaymentSelect);
            }
            break;

        case State::PaymentSelect:
        case State::PayCash:
        case State::PayOnlineQr:
            if (elapsed_ms() >= timings::PAYMENT_TIMEOUT_MS) {
                abandon();
            }
            break;

        case State::ThankYou:
            if (elapsed_ms() >= timings::THANK_YOU_MS) {
                finish_transaction();
            }
            break;

        case State::Abandoned:
            if (elapsed_ms() >= timings::ABANDONED_MS) {
                finish_transaction();
            }
            break;

        case State::RefundOwed:
            // Held longer than the other end-of-transaction screens because it
            // asks something of the customer rather than just informing them.
            if (elapsed_ms() >= timings::REFUND_OWED_MS) {
                finish_transaction();
            }
            break;

        case State::UtilityMenu:
            if (elapsed_ms() >= timings::UTILITY_MENU_MS) {
                log(LogLevel::INFORMATION,
                    "checkout: maintenance menu timed out");
                enter(State::Idle);
            }
            break;

        case State::SdResult:
            if (elapsed_ms() >= timings::SD_RESULT_MS) {
                enter(State::Idle);
            }
            break;

        case State::TareResult:
            // Kept live while it is up, so the number on screen is the cell's
            // current reading rather than the instant the tare finished.
            Display::update_tare_reading(scale::box_grams());
            if (elapsed_ms() >= timings::TARE_RESULT_MS) {
                enter(State::Idle);
            }
            break;

        case State::UselessButton:
            if (elapsed_ms() >= timings::USELESS_BUTTON_MS) {
                enter(State::Idle);
            }
            break;

        case State::Fault:
            // Held for a minimum time so an intermittent fault cannot strobe
            // the screen, then released once the cause has gone. The link being
            // healthy is the only condition currently checked; a fault raised
            // from the debug key therefore clears on its own after timings::FAULT_MIN_MS.
            if (elapsed_ms() >= timings::FAULT_MIN_MS && pi_link::is_healthy()) {
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
    transaction_baseline = basket::Inventory{};
    has_baseline = false;
    sale_announced = false;
    current_basket = basket::Basket{};
    owed_cents = 0;
    paid_cents = 0;
    paid_grams = 0.0;
    method = PaymentMethod::None;
    transaction_id = 0;
    fault_code = FAULT_NONE;
    greeted_name = nullptr;

    // Nothing is known about the shelf yet, and nothing needs to be. The
    // baseline is captured per transaction, from the scan requested when the
    // door opens, so the first customer after a reboot is measured against a
    // real reading rather than against an all-zero table.
    //
    // That is a change from the previous design, where the baseline carried
    // over between customers and started empty — which made the first purchase
    // after every boot look like a restock, and therefore free.

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
