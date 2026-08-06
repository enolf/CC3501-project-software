#include "peripherals/scale_task/scale_task.h"

#include <stdio.h>

#include "board.h"
#include "drivers/logging/logging.h"
#include "drivers/mass_sensor/mass_sensor.h"
#include "peripherals/coin_acceptor/coin_acceptor.h"
#include "peripherals/events/events.h"

namespace scale {

namespace {

// One HX711 and one classifier. File-scope statics rather than a class,
// following the course convention for a peripheral there is only ever one of
// (CLAUDE.md section 4).
MassSensor  sensor;
CoinAcceptor acceptor;

bool ready = false;
bool dump_raw = false;

/// The most recent reading straight off the cell, in grams above the HARDWARE
/// tare — which is to say, what is actually sitting in the box.
///
/// Kept separately from the coin acceptor's `level_grams()` because the two
/// answer different questions and only one of them is "how much money is in
/// there". See the note on box_grams().
double last_raw_grams = 0.0;

} // namespace

bool init()
{
    ready = sensor.init();
    if (!ready) {
        logf(LogLevel::ERROR,
             "scale: HX711 not responding on GP%d/GP%d - cash payment unavailable",
             HX711_DATA_PIN, HX711_CLK_PIN);
        return false;
    }

    // Taring blocks for about a second. Startup is the one place that is
    // acceptable, which is why it happens here and never again automatically.
    if (!sensor.tare()) {
        log(LogLevel::WARNING,
            "scale: tare failed, readings are relative to an unknown zero");
    }

    acceptor.begin();
    log(LogLevel::INFORMATION, "scale: HX711 ready");
    return true;
}

void run_scale()
{
    if (!ready) {
        return;
    }

    // poll() returns false immediately when the HX711 has not finished a new
    // conversion, which at 10 samples a second is almost every pass. Nothing
    // below runs on those.
    double grams = 0.0;
    if (!sensor.poll(grams)) {
        return;
    }
    last_raw_grams = grams;

    if (dump_raw) {
        printf("raw: %8.3f g\n", grams);
    }

    const CoinEvent result = acceptor.update(grams);

    switch (result.kind) {
        case CoinEventKind::CoinsAdded: {
            events::Event e(events::Kind::CoinAccepted);
            e.coin.cents       = result.cents;
            e.coin.one_dollar  = result.one_dollar;
            e.coin.two_dollar  = result.two_dollar;
            e.coin.delta_grams = (float)result.delta_grams;
            events::push(e);

            logf(LogLevel::INFORMATION, "scale: %ux $1 + %ux $2 = %dc (%+.2f g)",
                 result.one_dollar, result.two_dollar, result.cents,
                 result.delta_grams);
            break;
        }

        case CoinEventKind::Unrecognised: {
            // The mass settled somewhere no plausible handful of coins explains.
            // Deliberately not counted: crediting a guess here is how a washer
            // buys a drink.
            events::Event e(events::Kind::CoinRejected);
            e.coin.delta_grams = (float)result.delta_grams;
            events::push(e);

            logf(LogLevel::WARNING,
                 "scale: %+.2f g is not a recognised coin combination",
                 result.delta_grams);
            break;
        }

        case CoinEventKind::MassRemoved:
            // The box got lighter. Either it was emptied between transactions,
            // which is routine, or someone is fishing coins out mid-payment,
            // which is not. Reported rather than acted on: the mass gate will
            // simply stop being satisfied, so a customer cannot profit from it.
            logf(LogLevel::WARNING, "scale: mass removed (%+.2f g)",
                 result.delta_grams);
            break;

        case CoinEventKind::None:
        default:
            break;
    }
}

void begin_transaction()
{
    acceptor.begin();
}

bool is_ready()
{
    return ready;
}

bool retare()
{
    if (!ready) {
        return false;
    }

    const bool ok = sensor.tare();
    // The zero moved, so whatever the classifier thought the resting level was
    // is meaningless now.
    acceptor.begin();

    // And so is the last raw sample, which was measured against the OLD zero.
    // Cleared rather than left stale because the tare screen reads it back
    // immediately — before `run_scale()` has had a chance to poll again — and a
    // pre-tare number under the words "Tare successful" is worse than no number
    // at all. It is replaced with a real reading on the next sample, about a
    // tenth of a second later.
    last_raw_grams = 0.0;
    return ok;
}

void set_raw_dump(bool enabled)
{
    dump_raw = enabled;
}

bool raw_dump_enabled()
{
    return dump_raw;
}

double level_grams()
{
    return acceptor.level_grams();
}

double box_grams()
{
    return last_raw_grams;
}

} // namespace scale
