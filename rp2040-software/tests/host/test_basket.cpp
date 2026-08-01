// Tests for the catalogue, the inventory diff and the cash payment gate.

#include "test_support.h"

#include "peripherals/basket/basket.h"
#include "peripherals/catalogue/catalogue.h"
#include "peripherals/coin_acceptor/coin_acceptor.h"

using catalogue::Can;

namespace {

basket::Inventory make(uint8_t coke, uint8_t sprite, uint8_t fanta, uint8_t pasito)
{
    basket::Inventory inv;
    inv.count[static_cast<uint8_t>(Can::Coke)]   = coke;
    inv.count[static_cast<uint8_t>(Can::Sprite)] = sprite;
    inv.count[static_cast<uint8_t>(Can::Fanta)]  = fanta;
    inv.count[static_cast<uint8_t>(Can::Pasito)] = pasito;
    return inv;
}

uint8_t taken_of(const basket::Basket &b, Can can)
{
    return b.taken[static_cast<uint8_t>(can)];
}

} // namespace

void test_catalogue()
{
    testing::suite("catalogue");

    CHECK_EQ(catalogue::price_cents(Can::Coke), 200);
    CHECK_EQ(catalogue::price_cents(Can::Pasito), 200);
    testing::case_passed("every drink is $2.00");

    // Prices are cents, so $2.00 must be 200 and not 2 or 2000. The 2000 case
    // is not hypothetical: the original inventory header had exactly that bug,
    // which would have charged $20 a can.
    for (uint8_t i = 0; i < catalogue::CAN_COUNT; i++) {
        const uint32_t price = catalogue::price_cents(catalogue::from_index(i));
        CHECK(price >= 50 && price <= 1000);
    }
    testing::case_passed("all prices are plausible cent values");

    CHECK(catalogue::is_valid(Can::Pasito));
    CHECK(!catalogue::is_valid(static_cast<Can>(catalogue::CAN_COUNT)));
    CHECK_EQ(catalogue::price_cents(static_cast<Can>(99)), 0);
    testing::case_passed("out-of-range drink IDs are rejected, not indexed");
}

void test_diff()
{
    testing::suite("inventory diff");

    basket::Basket b;

    // --- One drink taken ---
    auto change = basket::diff(make(5, 5, 5, 5), make(4, 5, 5, 5), b);
    CHECK(change == basket::Change::ItemsTaken);
    CHECK_EQ(taken_of(b, Can::Coke), 1);
    CHECK_EQ(basket::item_count(b), 1);
    CHECK_EQ(basket::total_cents(b), 200);
    testing::case_passed("single can taken -> $2.00");

    // --- Several drinks, several types ---
    change = basket::diff(make(5, 5, 5, 5), make(3, 5, 4, 5), b);
    CHECK(change == basket::Change::ItemsTaken);
    CHECK_EQ(taken_of(b, Can::Coke), 2);
    CHECK_EQ(taken_of(b, Can::Fanta), 1);
    CHECK_EQ(basket::item_count(b), 3);
    CHECK_EQ(basket::total_cents(b), 600);
    testing::case_passed("2 Coke + 1 Fanta -> $6.00");

    // --- Door opened and closed, nothing taken ---
    change = basket::diff(make(5, 5, 5, 5), make(5, 5, 5, 5), b);
    CHECK(change == basket::Change::None);
    CHECK(basket::is_empty(b));
    CHECK_EQ(basket::total_cents(b), 0);
    testing::case_passed("no change -> empty basket, nothing owed");

    // --- Restock ---
    change = basket::diff(make(2, 2, 2, 2), make(6, 6, 6, 6), b);
    CHECK(change == basket::Change::Restocked);
    CHECK(basket::is_empty(b));
    CHECK_EQ(basket::total_cents(b), 0);
    testing::case_passed("restock -> nobody is charged");

    // --- Swap: one put back, another taken ---
    // A real customer behaviour, and the basket must contain only the drink
    // that left. Charging for the returned one would be theft in reverse.
    change = basket::diff(make(5, 5, 5, 5), make(6, 5, 4, 5), b);
    CHECK(change == basket::Change::Mixed);
    CHECK_EQ(taken_of(b, Can::Coke), 0);
    CHECK_EQ(taken_of(b, Can::Fanta), 1);
    CHECK_EQ(basket::total_cents(b), 200);
    testing::case_passed("swapped a Coke for a Fanta -> charged for the Fanta only");

    // --- The whole shelf ---
    change = basket::diff(make(1, 1, 1, 1), make(0, 0, 0, 0), b);
    CHECK(change == basket::Change::ItemsTaken);
    CHECK_EQ(basket::item_count(b), 4);
    CHECK_EQ(basket::total_cents(b), 800);
    testing::case_passed("emptied the fridge -> $8.00");

    // --- Empty shelf stays empty ---
    change = basket::diff(make(0, 0, 0, 0), make(0, 0, 0, 0), b);
    CHECK(change == basket::Change::None);
    testing::case_passed("empty fridge, no change");
}

void test_mass_gate()
{
    testing::suite("cash payment gate");

    const double ONE = CoinAcceptor::ONE_DOLLAR_GRAMS;   // 9.80 g
    const double TWO = CoinAcceptor::TWO_DOLLAR_GRAMS;   // 6.60 g

    // $2.00 owed. Exactly two ways to pay it: one $2 (6.60 g) or two $1
    // (19.60 g). Note they are 13 g apart, which is the whole reason this
    // works.
    CHECK(basket::check_mass(200, TWO) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(200, 2 * ONE) == basket::MassVerdict::Exact);
    testing::case_passed("$2.00 paid by one $2, or by two $1");

    // Nothing in the box yet.
    CHECK(basket::check_mass(200, 0.0) == basket::MassVerdict::Insufficient);
    CHECK(!basket::mass_gate_satisfied(200, 0.0));
    testing::case_passed("empty box does not satisfy $2.00");

    // Halfway: one $1 coin against a $2 debt.
    CHECK(basket::check_mass(200, ONE) == basket::MassVerdict::Insufficient);
    testing::case_passed("a single $1 does not pay a $2 drink");

    // Overpaying with two $2 coins ($4.00 for a $2.00 drink). This is the case
    // a naive "is the mass big enough" test gets WRONG: 13.20 g is less than
    // the 19.60 g of a correct two-$1 payment, yet it is worth twice as much.
    CHECK(basket::check_mass(200, 2 * TWO) == basket::MassVerdict::Overpaid);
    CHECK(basket::mass_gate_satisfied(200, 2 * TWO));
    testing::case_passed("two $2 coins overpay a $2 drink despite weighing less than two $1");

    // Tolerance: drift and per-coin error must not block a valid payment.
    CHECK(basket::check_mass(200, TWO + 1.0) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(200, TWO - 1.0) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(200, 2 * ONE + 1.0) == basket::MassVerdict::Exact);
    testing::case_passed("payment still accepted with +/- 1.0 g of drift");

    // GUARD: the tolerance must stay below half the gap between the two
    // single-coin masses, or their windows overlap and a correctly-paid $2 coin
    // gets scored as a $1 — see the long comment on MASS_GATE_TOLERANCE_GRAMS.
    //
    // Asserted as a RELATIONSHIP between the constants rather than against a
    // fixed number, deliberately. When the scale was recalibrated the $1 moved
    // from 9.80 g to 9.00 g and the safe limit moved with it, from 1.60 g to
    // 1.20 g; a hard-coded check would have kept passing while the tolerance
    // was quietly out of range. This form cannot.
    {
        const double single_coin_gap = ONE - TWO;
        CHECK(single_coin_gap > 0.0);
        CHECK(basket::MASS_GATE_TOLERANCE_GRAMS < single_coin_gap / 2.0);
        testing::case_passed("tolerance stays below half the single-coin gap");
    }

    // A mass matching no plausible coin combination at all.
    CHECK(basket::check_mass(200, 3.0) == basket::MassVerdict::Insufficient);
    testing::case_passed("a 3 g foreign object pays for nothing");

    // $4.00 owed (two drinks). Valid: two $2 (13.20), one $2 + two $1 (26.20),
    // four $1 (39.20).
    CHECK(basket::check_mass(400, 2 * TWO) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(400, TWO + 2 * ONE) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(400, 4 * ONE) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(400, TWO) == basket::MassVerdict::Insufficient);
    testing::case_passed("$4.00 accepts all three exact combinations, rejects $2.00");

    // An ambiguous reading. Two $1 (18.00 g, $2.00) and three $2 (19.80 g,
    // $6.00) are 1.80 g apart, so a reading at 18.90 g sits 0.90 g from each
    // and is consistent with both. The gate must assume the CHEAPER one, or it
    // would let someone owing $6.00 walk away having paid $2.00.
    CHECK(basket::check_mass(200, 18.90) == basket::MassVerdict::Exact);
    CHECK(basket::check_mass(600, 18.90) == basket::MassVerdict::Insufficient);
    testing::case_passed("an ambiguous reading is scored as the cheaper combination");

    // Nothing owed is trivially satisfied.
    CHECK(basket::check_mass(0, 0.0) == basket::MassVerdict::Exact);
    testing::case_passed("nothing owed needs no coins");
}
