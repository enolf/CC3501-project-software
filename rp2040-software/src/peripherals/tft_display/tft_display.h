#pragma once

#include <stdint.h>

#include "peripherals/basket/basket.h"

// The payment terminal's screen.
//
// The API is deliberately SEMANTIC: callers say what is happening, never how to
// draw it. Nothing outside this module has any idea LVGL exists, so the entire
// look of the terminal can be reworked without touching the state machine, and
// the state machine can be read without knowing anything about widgets.
//
// Scope is narrow on purpose (documentation.md section 4.4): this is a payment
// terminal and nothing else. No temperatures, no stock levels, no diagnostics —
// all of that belongs on the dashboard.
//
// Touch presses are turned into events (TouchCash, TouchOnline, TouchBack) and
// pushed onto the event queue. The button callbacks do nothing else, so the
// state machine remains the only code that decides what a press means.

class Display {
public:
    /// Bring up the ILI9341, LVGL and the touch input device.
    /// BLOCKS for roughly 750 ms in the panel's reset sequence.
    static void init();

    /// Service LVGL. Call every pass of the superloop.
    static void run();

    // --- One call per state of the checkout machine ---

    /// Nothing in progress. Black, by decision; see checkout::IDLE_SCREEN_STYLE.
    static void show_idle();

    /// An approved card was tapped: greet the holder and ask for the door.
    ///
    /// `holder_name` may be nullptr, which greets them without a name rather
    /// than printing "(null)". That is not a defensive nicety — it is the
    /// screen a CardApproved event gets when its UID no longer resolves to a
    /// row in the approved list.
    static void show_greeting(const char *holder_name);

    /// A card was read that is not on the approved list.
    static void show_access_denied();

    /// The fridge is open and the customer is choosing.
    static void show_selecting();

    /// The door has shut and the camera is recounting.
    static void show_recount();

    /// The basket and the total, with Cash and Online touch targets.
    static void show_payment_select(const basket::Basket &basket,
                                    uint32_t owed_cents);

    /// Live cash progress. Safe to call on every coin: the screen is only
    /// rebuilt on the first call, and later calls just update the numbers.
    static void show_cash_progress(uint32_t paid_cents, uint32_t owed_cents);

    /// Waiting for the Pi to return a Square payment link.
    static void show_online_waiting();

    /// The QR code for `url`, with the amount and a "pay cash instead" target.
    static void show_qr(const char *url, uint32_t owed_cents);

    /// Paid. Brief confirmation.
    ///
    /// Takes `owed_cents` as well so it can name the tip when somebody
    /// overpays. The difference is not derivable from `paid_cents` alone, and
    /// working it out at the call site would put the decision about what the
    /// screen says in the state machine rather than in the display.
    static void show_thanks(uint32_t paid_cents, uint32_t owed_cents);

    /// The transaction ended without payment.
    static void show_cancelled();

    /// Every drink was put back after coins had already gone in, so the
    /// customer is owed `paid_cents` and the machine has no way to return it.
    ///
    /// Takes the amount rather than just saying "see somebody" because the
    /// number is the whole point: it is what the customer has to be able to
    /// quote, and it is written to the log in the same breath so the two
    /// accounts can be reconciled.
    static void show_refund_owed(uint32_t paid_cents);

    /// Out of service, with a numeric code. Diagnostics go to the serial log,
    /// not the screen — customers get one clear message (decision D14).
    static void show_fault(uint16_t code);

    // --- The panel button ---------------------------------------------------

    /// The maintenance menu: TARE BOX on the left, WRITE SD on the right.
    ///
    /// Reuses TouchCash and TouchOnline for its two targets rather than adding
    /// events of its own. They are already "the left button" and "the right
    /// button" everywhere else, the state machine reads them in exactly one
    /// state at a time, and two more event kinds would have to be ignored by
    /// every other state for no gain.
    static void show_utility_menu();

    /// "Writing log to SD card". Put up BEFORE the write starts.
    ///
    /// The write blocks the superloop for as long as the card takes, so this
    /// has to be drawn and flushed before it begins — a screen queued behind a
    /// blocking write appears only after it has finished, which is precisely
    /// when it is no longer true.
    static void show_sd_writing();

    /// "Taring...". Same reasoning as show_sd_writing(): retare() blocks for
    /// about a second, so this is drawn and flushed before it is called.
    static void show_taring();

    /// The tare outcome and the reading it left behind.
    ///
    /// `grams` is shown whether or not it succeeded, because a tare that
    /// reports success and then reads nowhere near zero is the interesting
    /// failure — the cell is responding but not behaving.
    static void show_tare_result(bool ok, double grams);

    /// Refresh the number on the tare screen, without rebuilding it. Ignored
    /// unless that screen is the one currently up, so it is safe to call every
    /// pass. Same arrangement as the cash screen's running total.
    static void update_tare_reading(double grams);

    /// The outcome, and permission to take the card out.
    static void show_sd_result(bool ok, unsigned long lines);

    /// A short press. The button does nothing else, and saying so is better
    /// than a button that appears broken.
    static void show_useless_button();
};
