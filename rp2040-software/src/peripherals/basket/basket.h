#pragma once

#include <stdint.h>

#include "peripherals/catalogue/catalogue.h"

// Working out what the customer took, what it costs, and whether they have
// paid for it.
//
// This is the arithmetic at the centre of the whole system, and it is
// deliberately free of any hardware dependency — no GPIO, no display, no
// serial. That is what allows every rule in here to be tested exhaustively on a
// PC (tests/host/) rather than by standing at a fridge feeding it coins.

namespace basket {

/// A snapshot of what is on the shelf, indexed by catalogue::Can.
struct Inventory {
    uint8_t count[catalogue::CAN_COUNT] = {};
};

/// What was taken during one door-open cycle.
struct Basket {
    uint8_t taken[catalogue::CAN_COUNT] = {};
};

/// What the difference between two inventory snapshots turned out to mean.
enum class Change : uint8_t {
    None,        ///< Identical. Door opened and closed, nothing taken.
    ItemsTaken,  ///< Counts only went down. The normal purchase.
    Restocked,   ///< Counts only went up. Someone refilled the fridge.
    Mixed,       ///< Some up, some down — see the note on diff().
};

/// Compare the shelf before and after a door cycle.
///
/// `out` always receives only the items whose count went DOWN, because those
/// are the ones being paid for. Items whose count went up are returns or a
/// restock and are never charged.
///
/// Mixed is a real customer behaviour, not an error: swapping a Coke for a
/// Fanta puts one back and takes another, so one count rises while another
/// falls. It is reported separately from ItemsTaken purely so it can be logged
/// — the caller should charge for `out` in both cases. What it must NOT do is
/// treat Mixed as a fault and refuse the sale, which would fail the customer
/// for doing something entirely reasonable.
///
/// Returns the classification; `out` is always written, and is all-zero for
/// None and Restocked.
Change diff(const Inventory &previous, const Inventory &current, Basket &out);

/// Total price of a basket, in cents.
uint32_t total_cents(const Basket &basket);

/// How many individual drinks are in a basket.
uint8_t item_count(const Basket &basket);

/// True if nothing was taken.
bool is_empty(const Basket &basket);

// ---------------------------------------------------------------------------
// The cash payment gate
// ---------------------------------------------------------------------------
// Deciding "has this been paid for?" from the coin box mass, rather than from
// the running coin tally.
//
// WHY NOT JUST TRUST THE TALLY
// ----------------------------
// CoinAcceptor counts coins as they land, one settling window at a time. Miss
// one — two coins dropped together, a knock during settling, a coin that lands
// on edge — and the tally is silently wrong for the rest of the transaction.
// The total mass in the box does not care how the coins arrived; it is the same
// number however they got there. So the tally drives the number shown to the
// customer, and the mass decides whether they have paid.
//
// WHY THIS IS NOT SIMPLY "MASS >= SOMETHING"
// ------------------------------------------
// The $2 coin is LIGHTER than the $1 (6.60 g against 9.80 g), so mass does not
// increase with value and there is no threshold to compare against. Paying
// $2.00 with one $2 coin puts 6.60 g in the box; paying the same $2.00 with two
// $1 coins puts 19.60 g in. Both are correct payments, 13 g apart.
//
// So the question asked here is: given this mass, what is the LEAST the
// customer could possibly have put in? Taking the minimum over every coin
// combination consistent with the reading is what makes the answer safe — it
// can never overestimate what was paid, so it can never let someone underpay.

/// How far the measured mass may sit from a candidate combination and still be
/// accepted as that combination.
///
/// THERE IS A HARD UPPER LIMIT ON THIS VALUE AND IT IS 1.20 g. Do not raise it
/// past that thinking a looser tolerance is more forgiving — it is the exact
/// opposite, and the failure is not obvious.
///
/// The two single-coin masses are 6.60 g and 9.00 g, 2.40 g apart. Once the
/// tolerance exceeds half that gap their windows overlap, and a reading in the
/// overlap is consistent with both a $2 coin and a $1 coin. Because the verdict
/// takes the LEAST value consistent with the reading, such a reading is scored
/// as $1.00 — so a customer who correctly paid $2.00 with a single $2 coin is
/// told they still owe money, and no amount of waiting fixes it.
///
/// This is not hypothetical: it was written as 2.00 g first, and the test
/// "payment still accepted with drift" failed for precisely this reason.
///
/// The limit moved when the scale was recalibrated — it was 1.60 g while the $1
/// was thought to weigh 9.80 g, because the coins looked further apart than
/// they are. A test below asserts the relationship rather than the number, so
/// changing a coin mass cannot silently invalidate this again.
///
/// 1.00 g leaves 0.20 g of clear space between the two windows while still
/// absorbing more drift than CoinAcceptor's own 0.80 g per-event tolerance.
constexpr double MASS_GATE_TOLERANCE_GRAMS = 1.00;

/// The verdict on a coin box mass.
enum class MassVerdict : uint8_t {
    Insufficient,  ///< Not yet enough, or the mass matches no plausible coins.
    Exact,         ///< Matches the amount owed exactly.
    Overpaid,      ///< More than owed. Accepted; no change is given.
};

/// Decide whether `grams` in the box covers `owed_cents`.
///
/// `grams` is the mass added since the transaction started, not the absolute
/// contents of the box.
MassVerdict check_mass(uint32_t owed_cents, double grams);

/// Convenience: true when the customer may be sent to the Thank You screen.
bool mass_gate_satisfied(uint32_t owed_cents, double grams);

} // namespace basket
