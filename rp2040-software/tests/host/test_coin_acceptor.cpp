// Tests for the coin classifier, re-run under MAX_COINS_PER_EVENT = 2.
//
// The classifier previously allowed three coins per event. With the measured
// 9.80 g $1 that became unsafe: two $1 (19.60 g) and three $2 (19.80 g) sit
// 0.20 g apart, well inside the 0.80 g match tolerance, so one reading matched
// two different answers. These tests exercise the two-coin behaviour and pin
// down what happens at the boundary.

#include "test_support.h"

#include "peripherals/coin_acceptor/coin_acceptor.h"

namespace {

constexpr double ONE = CoinAcceptor::ONE_DOLLAR_GRAMS;   // 9.80 g
constexpr double TWO = CoinAcceptor::TWO_DOLLAR_GRAMS;   // 6.60 g

/// Feed a steady reading until the acceptor adopts it as the resting level.
/// The first settled reading only establishes the reference point and produces
/// no event, which is why every test starts with this.
void settle_baseline(CoinAcceptor &acceptor, double grams = 0.0)
{
    for (int i = 0; i < CoinAcceptor::SETTLE_SAMPLES + 2; i++) {
        acceptor.update(grams);
    }
}

/// Feed a steady reading and return the first event it produces, if any.
/// Real hardware delivers many samples at the same level; only the sample that
/// completes a settled change reports anything.
CoinEvent settle_at(CoinAcceptor &acceptor, double grams)
{
    CoinEvent first;
    for (int i = 0; i < CoinAcceptor::SETTLE_SAMPLES + 2; i++) {
        const CoinEvent e = acceptor.update(grams);
        if (e.kind != CoinEventKind::None && first.kind == CoinEventKind::None) {
            first = e;
        }
    }
    return first;
}

} // namespace

void test_coin_acceptor()
{
    testing::suite("coin classification");

    // --- 1. A single $1 ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, ONE);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        CHECK_EQ(e.two_dollar, 0);
        CHECK_EQ(e.cents, 100);
        CHECK_EQ(acc.cents_total(), 100);
        testing::case_passed("one $1 coin");
    }

    // --- 2. A single $2 ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, TWO);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.two_dollar, 1);
        CHECK_EQ(e.cents, 200);
        testing::case_passed("one $2 coin");
    }

    // --- 3. Two $1 landing together ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 2 * ONE);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 2);
        CHECK_EQ(e.cents, 200);
        testing::case_passed("two $1 in one settling window");
    }

    // --- 4. Two $2 landing together ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 2 * TWO);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.two_dollar, 2);
        CHECK_EQ(e.cents, 400);
        testing::case_passed("two $2 in one settling window");
    }

    // --- 5. One of each together ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, ONE + TWO);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        CHECK_EQ(e.two_dollar, 1);
        CHECK_EQ(e.cents, 300);
        testing::case_passed("a $1 and a $2 together");
    }

    // --- 6. A worn $2, light by 0.15 g ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 6.45);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.two_dollar, 1);
        testing::case_passed("a worn 6.45 g $2 is still a $2");
    }

    // --- 7. A worn $1, light by 0.30 g ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 9.50);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        testing::case_passed("a worn 9.50 g $1 is still a $1");
    }

    // --- 8. A foreign object ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 4.0);
        CHECK(e.kind == CoinEventKind::Unrecognised);
        CHECK_EQ(acc.cents_total(), 0);
        testing::case_passed("a 4 g washer is rejected and not counted");
    }

    // --- 9. Slow thermal drift ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 1.2);
        CHECK(e.kind == CoinEventKind::None);
        CHECK_EQ(acc.cents_total(), 0);
        testing::case_passed("1.2 g of drift produces no event");
    }

    // --- 10. A ringing beam ---
    // While the box is oscillating the window never settles, so nothing may be
    // reported. The coin is only counted once the ringing stops.
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);

        const double ringing[] = { 2.0, 14.0, 5.0, 12.0, 7.0, 11.0 };
        bool fired_while_ringing = false;
        for (double sample : ringing) {
            if (acc.update(sample).kind != CoinEventKind::None) {
                fired_while_ringing = true;
            }
        }
        CHECK(!fired_while_ringing);
        CHECK(!acc.is_settled());

        const CoinEvent e = settle_at(acc, ONE);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        testing::case_passed("a ringing beam reports nothing until it stops");
    }

    // --- 11. The box being emptied ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc, 50.0);
        const CoinEvent e = settle_at(acc, 0.0);
        CHECK(e.kind == CoinEventKind::MassRemoved);
        CHECK(e.delta_grams < 0.0);
        testing::case_passed("emptying the box reports MassRemoved");
    }

    // --- 12. Coins arriving one at a time ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);

        CoinEvent e = settle_at(acc, TWO);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        e = settle_at(acc, TWO + ONE);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        e = settle_at(acc, TWO + ONE + TWO);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.two_dollar, 1);

        CHECK_EQ(acc.cents_total(), 500);
        testing::case_passed("$2 then $1 then $2 accumulates to $5.00");
    }

    // --- 13. begin() clears the tally ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        settle_at(acc, ONE);
        CHECK_EQ(acc.cents_total(), 100);

        acc.begin();
        CHECK_EQ(acc.cents_total(), 0);
        CHECK(!acc.is_settled());
        testing::case_passed("begin() resets the tally between transactions");
    }

    // --- 14. An unrecognised step does not repeat forever ---
    // The level is adopted even when the change cannot be explained, so the
    // same unexplained mass is not re-reported on every later sample.
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        CHECK(settle_at(acc, 4.0).kind == CoinEventKind::Unrecognised);
        CHECK(settle_at(acc, 4.0).kind == CoinEventKind::None);
        testing::case_passed("an unrecognised mass is reported once, not repeatedly");
    }

    // --- 15. A coin after a rejection still counts ---
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        settle_at(acc, 4.0);                       // washer
        const CoinEvent e = settle_at(acc, 4.0 + ONE);
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.one_dollar, 1);
        CHECK_EQ(acc.cents_total(), 100);
        testing::case_passed("a real coin after a washer is still counted");
    }

    // --- 16. THE property that makes the classifier safe ---
    //
    // Every mass reachable within MAX_COINS_PER_EVENT must be separated from
    // every other by more than twice MATCH_TOLERANCE_GRAMS. If any two came
    // closer, a single reading could sit within range of both and the answer
    // would depend on rounding.
    //
    // Computed from the constants rather than written down, so that changing a
    // coin mass or the coin cap fails HERE — which is exactly what was needed
    // when the $1 was briefly believed to weigh 9.80 g and two $1 collided with
    // three $2 at 0.20 g apart.
    {
        double reachable[32];
        int count = 0;
        for (uint8_t total = 1; total <= CoinAcceptor::MAX_COINS_PER_EVENT; total++) {
            for (uint8_t ones = 0; ones <= total; ones++) {
                reachable[count++] = ones * ONE + (total - ones) * TWO;
            }
        }

        double smallest_gap = 1e9;
        for (int i = 0; i < count; i++) {
            for (int j = i + 1; j < count; j++) {
                const double gap = fabs(reachable[i] - reachable[j]);
                if (gap > 0.0001 && gap < smallest_gap) {
                    smallest_gap = gap;
                }
            }
        }

        CHECK(smallest_gap > 2.0 * CoinAcceptor::MATCH_TOLERANCE_GRAMS);
        CHECK_NEAR(smallest_gap, 1.80, 0.001);   // 18.00 g vs 19.80 g
        testing::case_passed("no two reachable masses are within twice the match tolerance");
    }

    // --- 17. Three coins at once ---
    // Three $2 landing inside one settling window weigh 19.80 g. With the $1 at
    // its true 9.00 g the nearest rival is two $1 at 18.00 g, 1.80 g away, so
    // this is classified correctly rather than ambiguously.
    //
    // This case is the reason MAX_COINS_PER_EVENT is 3: a customer paying $6.00
    // by dropping three coins together is credited properly. While the $1 was
    // wrongly believed to be 9.80 g the cap had to be 2, and this same deposit
    // was under-credited as $2.00.
    {
        CoinAcceptor acc;
        acc.begin();
        settle_baseline(acc);
        const CoinEvent e = settle_at(acc, 3 * TWO);   // 19.80 g = $6.00
        CHECK(e.kind == CoinEventKind::CoinsAdded);
        CHECK_EQ(e.two_dollar, 3);
        CHECK_EQ(e.one_dollar, 0);
        CHECK_EQ(e.cents, 600);
        testing::case_passed("three $2 dropped together are credited as $6.00");
    }
}
