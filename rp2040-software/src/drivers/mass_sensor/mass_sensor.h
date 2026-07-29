#pragma once

#include <stdint.h>

#include "pico-scale/extern/hx711-pico-c/include/hx711.h"

/// Mass measurement from the money box scale: a TAL221 beam load cell feeding
/// an HX711 24-bit amplifier, which the RP2040 clocks out using a PIO state
/// machine. Wiring and calibration live in board.h.
///
/// Two design decisions are worth understanding before using this class.
///
/// 1. READS ARE NON-BLOCKING. The HX711 produces samples at a fixed rate set
///    by its RATE pin -- 10 per second on this board. A blocking read would
///    therefore stall the caller for up to 100 ms at a time, which the rest of
///    the system cannot afford once the display (LVGL wants servicing every
///    few milliseconds) and the RFID reader share the same loop. Instead,
///    poll() returns false immediately when no new sample has arrived, so the
///    caller keeps control and simply asks again next time round.
///
/// 2. READINGS ARE RELATIVE, NOT ABSOLUTE. poll() reports a signed offset in
///    grams from a baseline captured by tare(), not a true weight. This is
///    deliberate: the money box's own weight, and any coins already sitting in
///    it from an earlier payment, drop straight out of the arithmetic. It also
///    means the zero-offset half of the load cell calibration never has to be
///    correct or kept up to date -- only the counts-per-gram scale factor
///    matters, and that is a property of the cell, not of how it is loaded.
class MassSensor {
public:
    /// Samples averaged to establish the zero baseline. At 10 samples/second
    /// this is one second of averaging, which is enough to take the sample
    /// noise out of the baseline without making startup feel stalled.
    static constexpr uint16_t DEFAULT_TARE_SAMPLES = 10;

    /// Longest tare() will wait for DEFAULT_TARE_SAMPLES to arrive before
    /// giving up. Generous compared to the ~1 s the samples should take, so a
    /// slow start does not fail, but bounded so a dead HX711 cannot hang the
    /// caller forever.
    static constexpr uint32_t TARE_TIMEOUT_MS = 5000;

    /// Configure the PIO reader, power up the amplifier and confirm it is
    /// actually talking. Returns false if no sample arrives within the
    /// settling window (the usual cause is a wiring or power fault -- the
    /// HX711's DOUT line simply stays high and no data is ever clocked out),
    /// or if the first sample comes back saturated, which means the load cell
    /// bridge is disconnected or the cell is grossly overloaded.
    ///
    /// Safe to call repeatedly: a failed attempt can just be retried.
    bool init();

    /// Stop the PIO state machine and release the HX711.
    void close();

    /// Fetch the next raw sample if one is ready. Returns false immediately
    /// when the HX711 has not produced a new conversion yet.
    bool poll_raw(int32_t &counts_out);

    /// Fetch the next sample as grams relative to the tare baseline. Returns
    /// false immediately when no new sample is ready.
    bool poll(double &grams_out);

    /// Average `samples` readings and adopt the result as the zero point.
    /// BLOCKS for roughly samples/rate seconds, so call it during setup or
    /// between transactions, never inside a time-critical loop. Returns false
    /// if no samples arrived at all within TARE_TIMEOUT_MS.
    bool tare(uint16_t samples = DEFAULT_TARE_SAMPLES);

    /// Discard any samples the PIO has already queued. Called internally after
    /// init() and tare() so the next poll() returns a genuinely fresh reading
    /// rather than one captured before the baseline changed.
    void drain();

    /// Convert a raw count difference to grams using the board's calibration.
    double counts_to_grams(int32_t counts) const;

    /// The raw count currently treated as zero.
    int32_t baseline_counts() const { return m_baseline; }

    /// True once init() has succeeded.
    bool is_ready() const { return m_ready; }

private:
    hx711_t m_hx = {};
    int32_t m_baseline = 0;
    bool    m_ready = false;
};
