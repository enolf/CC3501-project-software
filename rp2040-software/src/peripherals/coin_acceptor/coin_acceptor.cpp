#include "peripherals/coin_acceptor/coin_acceptor.h"

#include <math.h>

void CoinAcceptor::begin()
{
    m_filled = 0;
    m_next = 0;
    m_level = 0.0;
    m_have_level = false;
    m_settled = false;
    m_one_dollar = 0;
    m_two_dollar = 0;
}

int CoinAcceptor::cents_total() const
{
    return static_cast<int>(m_one_dollar) * ONE_DOLLAR_CENTS +
           static_cast<int>(m_two_dollar) * TWO_DOLLAR_CENTS;
}

bool CoinAcceptor::classify(double delta, uint8_t &one_dollar, uint8_t &two_dollar)
{
    // Walk every combination of up to MAX_COINS_PER_EVENT coins and keep the
    // one whose mass is closest to the measured step. Starting `best_error` at
    // the tolerance means anything worse than the tolerance is never accepted,
    // so this both finds the best fit and rejects a step that fits nothing.
    //
    // The search space is tiny (a handful of pairs), so a plain exhaustive
    // sweep is clearer here than anything cleverer, and costs nothing.
    double best_error = MATCH_TOLERANCE_GRAMS;
    bool found = false;

    for (uint8_t total = 1; total <= MAX_COINS_PER_EVENT; total++) {
        for (uint8_t ones = 0; ones <= total; ones++) {
            const uint8_t twos = static_cast<uint8_t>(total - ones);

            const double expected = ones * ONE_DOLLAR_GRAMS +
                                    twos * TWO_DOLLAR_GRAMS;
            const double error = fabs(delta - expected);

            if (error < best_error) {
                best_error = error;
                one_dollar = ones;
                two_dollar = twos;
                found = true;
            }
        }
    }

    return found;
}

CoinEvent CoinAcceptor::update(double grams)
{
    CoinEvent event;

    // --- Slide the newest reading into the rolling window ---
    m_window[m_next] = grams;
    m_next = static_cast<uint8_t>((m_next + 1) % SETTLE_SAMPLES);
    if (m_filled < SETTLE_SAMPLES) {
        m_filled++;
    }

    if (m_filled < SETTLE_SAMPLES) {
        m_settled = false;
        return event;   // not enough history yet
    }

    // --- Is the mass holding still? ---
    // Peak-to-peak across the window, rather than a variance or an average, so
    // a single stray reading is enough to withhold a decision. While a coin is
    // striking the box and the beam is ringing, the spread stays wide and no
    // event can be produced -- which is precisely the behaviour wanted.
    double lowest = m_window[0];
    double highest = m_window[0];
    double sum = 0.0;

    for (uint8_t i = 0; i < SETTLE_SAMPLES; i++) {
        const double value = m_window[i];
        if (value < lowest)  lowest = value;
        if (value > highest) highest = value;
        sum += value;
    }

    if ((highest - lowest) > SETTLE_BAND_GRAMS) {
        m_settled = false;
        return event;   // still moving
    }

    m_settled = true;

    // The window has already been shown to be tight, so the mean is a fine
    // estimate of the resting mass and is cheaper than sorting for a median.
    const double settled = sum / SETTLE_SAMPLES;

    // --- First settled reading establishes the reference point ---
    if (!m_have_level) {
        m_level = settled;
        m_have_level = true;
        return event;
    }

    const double delta = settled - m_level;

    if (fabs(delta) < EVENT_THRESHOLD_GRAMS) {
        return event;   // unchanged, within drift
    }

    // Something real happened. Adopt the new level either way, so that one
    // unexplained change does not leave every later reading being measured
    // against a stale reference and reported over and over.
    m_level = settled;
    event.delta_grams = delta;

    if (delta < 0.0) {
        event.kind = CoinEventKind::MassRemoved;
        return event;
    }

    uint8_t ones = 0;
    uint8_t twos = 0;
    if (!classify(delta, ones, twos)) {
        event.kind = CoinEventKind::Unrecognised;
        return event;
    }

    m_one_dollar = static_cast<uint16_t>(m_one_dollar + ones);
    m_two_dollar = static_cast<uint16_t>(m_two_dollar + twos);

    event.kind = CoinEventKind::CoinsAdded;
    event.one_dollar = ones;
    event.two_dollar = twos;
    event.cents = ones * ONE_DOLLAR_CENTS + twos * TWO_DOLLAR_CENTS;
    return event;
}
