#pragma once

#include <stdbool.h>

// The money box scale: turns load cell readings into coin events.
//
// Joins two pieces that already exist and knew nothing about each other —
// drivers/mass_sensor (the HX711, which reports grams) and
// peripherals/coin_acceptor (which turns changes in grams into coin counts) —
// and publishes the result as events for the state machine.
//
// Everything here is non-blocking except init(). The HX711 produces only 10
// samples a second, so run_scale() does nothing at all on the vast majority of
// passes through the superloop.

namespace scale {

/// Bring up the HX711 and establish the zero point.
///
/// BLOCKS for roughly a second while taring, which is fine at startup and
/// nowhere else. Returns false if the amplifier does not respond or the load
/// cell reads saturated; the system still runs, but cash payment will not work.
bool init();

/// Poll for a new sample and raise CoinAccepted / CoinRejected events.
/// Call every pass of the superloop.
void run_scale();

/// Forget the current resting mass and start a fresh tally.
///
/// Call at the START of a transaction, not at the end: the next settled reading
/// becomes the new reference point, so whatever coins are already in the box
/// drop out of the arithmetic.
///
/// Note this is NOT a re-tare. Taring blocks for about a second while it
/// averages, which would freeze the display; this just tells the classifier to
/// forget where it was, which costs nothing.
void begin_transaction();

/// True once the HX711 has been found and is producing samples.
bool is_ready();

/// Re-establish the zero point. BLOCKS for about a second — only safe when
/// nothing is in progress. Returns false if no samples arrived.
bool retare();

/// Print every sample as it arrives, for checking the noise floor and for
/// weighing coins during calibration.
void set_raw_dump(bool enabled);
bool raw_dump_enabled();

/// Mass currently accepted as the resting level, in grams above the coin
/// acceptor's per-payment baseline.
///
/// NOT the contents of the money box. `begin_transaction()` re-baselines at the
/// start of every payment so that coins already in the box drop out of the
/// arithmetic — which is exactly right for deciding whether THIS customer has
/// paid, and exactly wrong for anything asking how much money is in there. Use
/// `box_grams()` for that.
double level_grams();

/// What is actually sitting in the money box, in grams above the HARDWARE tare.
///
/// This is the number the dashboard's money-box gauge and the cash
/// reconciliation want: the raw cell reading, unaffected by the per-payment
/// baseline. Reporting `level_grams()` there made the gauge read ~0 with a box
/// full of coins, because they had all gone in before the current baseline.
///
/// ZERO MEANS "SAME AS AT TARE TIME", NOT "EMPTY". The tare is taken once at
/// startup, so if the box had coins in it when the board booted, they are
/// invisible to this and every reading is short by their mass. Boot with an
/// empty box, or re-tare with `retare()` once it is emptied.
double box_grams();

} // namespace scale
