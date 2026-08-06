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
    static void show_thanks(uint32_t paid_cents);

    /// The transaction ended without payment.
    static void show_cancelled();

    /// Out of service, with a numeric code. Diagnostics go to the serial log,
    /// not the screen — customers get one clear message (decision D14).
    static void show_fault(uint16_t code);
};
