#pragma once

#include <stdint.h>

/// What the last call to CoinAcceptor::update() concluded.
enum class CoinEventKind : uint8_t {
    None,          ///< Nothing has changed, or the reading has not settled yet.
    CoinsAdded,    ///< One or more coins landed and were identified.
    Unrecognised,  ///< The mass changed and settled, but not by any credible
                   ///< combination of coins (a foreign object, a knock, or
                   ///< more coins at once than the classifier will attempt).
    MassRemoved,   ///< The mass went down: the box was emptied, lifted, or a
                   ///< coin bounced back out.
};

/// A single settled change in the money box contents.
struct CoinEvent {
    CoinEventKind kind = CoinEventKind::None;
    uint8_t one_dollar = 0;   ///< $1 coins in THIS event (not the running total)
    uint8_t two_dollar = 0;   ///< $2 coins in THIS event
    int     cents = 0;        ///< value of THIS event
    double  delta_grams = 0;  ///< measured mass change that produced it
};

/// Works out which Australian gold coins have been dropped into the money box,
/// from load cell readings alone.
///
/// WHY THIS IS NOT JUST DIVISION
/// -----------------------------
/// The $1 coin weighs 9.00 g and the $2 coin weighs 6.60 g -- the more
/// valuable coin is the *lighter* one. Total mass therefore does not increase
/// with total value, and there is no scale factor that converts grams to
/// dollars. Every reading has to be resolved into a specific combination of
/// coins.
///
/// HOW IT WORKS
/// ------------
/// Rather than trying to solve the whole pile at once, this class watches for
/// each individual change. When the reading settles at a new level, the
/// *difference* from the previous level is matched against the small set of
/// masses a handful of coins could produce. A single coin is either +9.0 g or
/// +6.6 g, which are 2.4 g apart -- a wide margin compared to the sub-gram
/// noise of the sensor, and one that stays constant no matter how full the box
/// already is. Solving the whole accumulated pile instead would get steadily
/// harder as coins piled up, because the possible totals crowd together onto a
/// 0.6 g grid.
///
/// This class deals only in grams and coin counts. It knows nothing about the
/// load cell, the display, or what is being bought, which keeps the awkward
/// part of the problem -- the classification -- testable off hardware by
/// feeding update() a synthetic sequence of readings.
///
/// FITTING INTO THE WIDER SYSTEM
/// -----------------------------
/// cents_total() is the number the checkout logic will compare against the
/// amount owed. Note that the eventual "has the customer paid?" decision
/// should NOT simply trust this running total: for a known amount owed, the
/// valid ways to pay it differ in mass by 11.4 g at a time, so checking the
/// total mass against that short list is a far more robust test than trusting
/// that every individual coin was tracked correctly. The running total is for
/// showing the customer their progress; the mass check is for deciding whether
/// they have paid. That check belongs in the checkout state machine, once it
/// exists, and needs the amount owed -- which is why it is not here.
class CoinAcceptor {
public:
    // --- Coin properties (Royal Australian Mint nominal masses) ---
    static constexpr double ONE_DOLLAR_GRAMS = 9.80;
    static constexpr double TWO_DOLLAR_GRAMS = 6.60;
    static constexpr int    ONE_DOLLAR_CENTS = 100;
    static constexpr int    TWO_DOLLAR_CENTS = 200;

    /// How many coins the classifier will try to explain in one event. Coins
    /// dropped in quick succession can land inside the same settling window
    /// and be seen as a single step, so this must be more than one; but the
    /// possible masses crowd together as the count rises, so it must not be
    /// large either. Three is a workable compromise.
    static constexpr uint8_t MAX_COINS_PER_EVENT = 3;

    /// How far a measured step may sit from a candidate combination and still
    /// be accepted as that combination.
    ///
    /// Across every 1-to-3 coin combination the closest two possible masses
    /// are 18.0 g (two $1) and 19.8 g (three $2), just 1.8 g apart. Keeping
    /// the tolerance below half that gap guarantees a step can never be within
    /// range of two different answers at once, so a match is always
    /// unambiguous. For the common single-coin case the nearest rival is a
    /// full 2.4 g away, so there is considerably more margin in practice.
    static constexpr double MATCH_TOLERANCE_GRAMS = 0.80;

    /// Consecutive readings that must agree before the mass is called settled.
    /// At 10 samples/second this is half a second of stability, which comes on
    /// top of the ~400 ms the HX711 itself takes to finish tracking a step --
    /// so a coin is reported roughly a second after it lands. Requiring this
    /// run of agreement is what rejects the impact transient as the coin
    /// strikes the box and the beam rings.
    static constexpr uint8_t SETTLE_SAMPLES = 5;

    /// Maximum spread across those readings for them to count as agreeing.
    /// Must be comfortably above the sensor's noise floor (otherwise it never
    /// settles) and comfortably below the smallest coin (otherwise it settles
    /// mid-transient). Tune against the raw dump if the box is in a noisy spot.
    static constexpr double SETTLE_BAND_GRAMS = 0.50;

    /// How much the settled mass must differ from the accepted level before it
    /// is treated as a real change rather than drift. Set well below the
    /// lightest coin (6.60 g) so no coin is ever missed, but well above the
    /// noise floor so slow thermal drift does not manufacture events.
    static constexpr double EVENT_THRESHOLD_GRAMS = 3.00;

    /// Reset to an empty tally and forget the current level. Call at the start
    /// of each payment: the next settled reading becomes the new reference
    /// point, so whatever is already in the box is ignored from then on.
    void begin();

    /// Feed one reading, in grams relative to the scale's tare baseline. Call
    /// this once for every sample the sensor produces. Most calls return None;
    /// an event is only reported on the sample that completes a settled change.
    CoinEvent update(double grams);

    uint16_t one_dollar_count() const { return m_one_dollar; }
    uint16_t two_dollar_count() const { return m_two_dollar; }

    /// Running value of everything identified since begin().
    int cents_total() const;

    /// Mass currently accepted as the resting level, in grams above baseline.
    double level_grams() const { return m_level; }

    /// True when the recent readings agree closely enough to be trusted.
    bool is_settled() const { return m_settled; }

private:
    /// Try to explain `delta` grams as 1..MAX_COINS_PER_EVENT coins. On
    /// success fills the counts and returns true.
    static bool classify(double delta, uint8_t &one_dollar, uint8_t &two_dollar);

    double  m_window[SETTLE_SAMPLES] = {};
    uint8_t m_filled = 0;      ///< samples in the window so far (saturates)
    uint8_t m_next = 0;        ///< next slot to overwrite

    double m_level = 0.0;      ///< last accepted resting mass
    bool   m_have_level = false;
    bool   m_settled = false;

    uint16_t m_one_dollar = 0;
    uint16_t m_two_dollar = 0;
};
