#include "peripherals/basket/basket.h"

#include <math.h>

// The coin masses and values live in CoinAcceptor and are used from there
// rather than copied, so there is exactly one definition of what a $1 weighs.
// That header is pure arithmetic with no hardware dependency, which is what
// makes including it here safe for the host tests.
#include "peripherals/coin_acceptor/coin_acceptor.h"

namespace basket {

Change diff(const Inventory &previous, const Inventory &current, Basket &out)
{
    out = Basket{};

    bool any_taken = false;
    bool any_added = false;

    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        // Signed arithmetic deliberately: both counts are uint8_t, and
        // `current - previous` on unsigned types wraps to a huge positive
        // number when the shelf loses a can, which is the exact case this
        // function exists to detect.
        const int16_t before = static_cast<int16_t>(previous.count[i]);
        const int16_t after  = static_cast<int16_t>(current.count[i]);
        const int16_t removed = static_cast<int16_t>(before - after);

        if (removed > 0) {
            out.taken[i] = static_cast<uint8_t>(removed);
            any_taken = true;
        } else if (removed < 0) {
            // Put back or restocked. Never charged for, so it does not go in
            // the basket — but it is why Mixed exists as a separate answer.
            any_added = true;
        }
    }

    if (any_taken && any_added) {
        return Change::Mixed;
    }
    if (any_taken) {
        return Change::ItemsTaken;
    }
    if (any_added) {
        return Change::Restocked;
    }
    return Change::None;
}

uint32_t total_cents(const Basket &basket)
{
    uint32_t total = 0;
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        total += static_cast<uint32_t>(basket.taken[i]) *
                 catalogue::price_cents(catalogue::from_index(i));
    }
    return total;
}

uint8_t item_count(const Basket &basket)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        count = static_cast<uint8_t>(count + basket.taken[i]);
    }
    return count;
}

bool is_empty(const Basket &basket)
{
    return item_count(basket) == 0;
}

MassVerdict check_mass(uint32_t owed_cents, double grams)
{
    if (owed_cents == 0) {
        return MassVerdict::Exact;   // nothing to pay for
    }

    if (grams <= 0.0) {
        return MassVerdict::Insufficient;
    }

    const double one_g  = CoinAcceptor::ONE_DOLLAR_GRAMS;
    const double two_g  = CoinAcceptor::TWO_DOLLAR_GRAMS;
    const int    one_c  = CoinAcceptor::ONE_DOLLAR_CENTS;
    const int    two_c  = CoinAcceptor::TWO_DOLLAR_CENTS;

    // Highest coin count worth considering: any more than this and even the
    // lightest coin would overshoot the measured mass. Bounds the search
    // without imposing an arbitrary limit on how much someone may pay.
    const double budget = grams + MASS_GATE_TOLERANCE_GRAMS;
    const int max_ones = static_cast<int>(budget / one_g);
    const int max_twos = static_cast<int>(budget / two_g);

    // Find the SMALLEST value among every coin combination whose mass matches
    // the reading. Minimum, not maximum: if a mass is ambiguous — and it can
    // be, since two $1 (19.60 g) and three $2 (19.80 g) are only 0.20 g apart —
    // then assuming the cheaper interpretation is the only safe choice. It can
    // never credit the customer with more than they actually put in.
    bool matched = false;
    uint32_t least_value_cents = 0;

    for (int ones = 0; ones <= max_ones; ones++) {
        for (int twos = 0; twos <= max_twos; twos++) {
            if (ones == 0 && twos == 0) {
                continue;
            }

            const double mass = ones * one_g + twos * two_g;
            if (fabs(mass - grams) > MASS_GATE_TOLERANCE_GRAMS) {
                continue;
            }

            const uint32_t value =
                static_cast<uint32_t>(ones * one_c + twos * two_c);

            if (!matched || value < least_value_cents) {
                least_value_cents = value;
                matched = true;
            }
        }
    }

    if (!matched) {
        // The mass matches no combination of coins at all. Either the customer
        // is still partway through paying and the box is between valid totals,
        // or something that is not a coin is in there. Either way, not paid.
        return MassVerdict::Insufficient;
    }

    if (least_value_cents < owed_cents) {
        return MassVerdict::Insufficient;
    }
    if (least_value_cents == owed_cents) {
        return MassVerdict::Exact;
    }
    return MassVerdict::Overpaid;
}

bool mass_gate_satisfied(uint32_t owed_cents, double grams)
{
    const MassVerdict verdict = check_mass(owed_cents, grams);
    return verdict == MassVerdict::Exact || verdict == MassVerdict::Overpaid;
}

} // namespace basket
