#include "drivers/mass_sensor/mass_sensor.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include "board.h"
#include "drivers/logging/logging.h"

// common.h only -- deliberately NOT hx711_reader.pio.h. That header *defines*
// (rather than declares) its init functions and carries no extern "C" guard,
// so including it from more than one C++ file gives the linker two definitions
// of the same symbol. load_cell.cpp already includes it. Everything needed
// here comes from hx711_get_default_config(), which fills in the PIO program
// and its init callbacks for us.
#include "pico-scale/extern/hx711-pico-c/include/common.h"

namespace {
    // How long init() waits for the amplifier's first conversion before
    // declaring it absent. hx711_get_settling_time() reports the datasheet
    // figure for the configured rate (400 ms at 10 Hz); doubling it leaves
    // room for a slow power-up without waiting so long that a genuinely dead
    // sensor takes an age to report.
    constexpr uint32_t PRESENCE_TIMEOUT_MULTIPLIER = 2;

    constexpr uint32_t MS_TO_US = 1000;
}

bool MassSensor::init()
{
    // Start from the library's defaults, which carry the PIO instance, the
    // reader program and the init callbacks, then override only the wiring.
    // Pin numbers come from board.h -- never hard-coded here.
    hx711_config_t cfg;
    hx711_get_default_config(&cfg);
    cfg.clock_pin = HX711_CLK_PIN;
    cfg.data_pin  = HX711_DATA_PIN;

    hx711_init(&m_hx, &cfg);

    // Gain 128 selects channel A, the high-gain input the load cell bridge is
    // wired to. The HX711 needs a settling period after power-up before its
    // output is meaningful.
    hx711_power_up(&m_hx, hx711_gain_128);
    hx711_wait_settle(HX711_RATE);

    // Presence check. hx711_init() cannot fail and the HX711 has no ID
    // register to interrogate, so the only way to know it is really there is
    // to insist on an actual conversion. A missing or unpowered amplifier
    // never drives DOUT low, so no sample is ever clocked out and this times
    // out -- which is exactly the wiring fault we want to catch at startup
    // rather than halfway through a payment.
    const uint32_t timeout_us =
        hx711_get_settling_time(HX711_RATE) * PRESENCE_TIMEOUT_MULTIPLIER * MS_TO_US;

    int32_t first = 0;
    if (!hx711_get_value_timeout(&m_hx, &first, timeout_us)) {
        log(LogLevel::ERROR, "HX711: no conversion - check DOUT/SCK wiring and power");
        hx711_close(&m_hx);
        m_ready = false;
        return false;
    }

    // A saturated first reading means the converter is talking but the analog
    // side is not sane: usually the bridge is open circuit (a broken load cell
    // wire) or the cell is loaded well past its range. Either way the readings
    // would be meaningless, so fail here instead of reporting nonsense grams.
    if (hx711_is_min_saturated(first) || hx711_is_max_saturated(first)) {
        log(LogLevel::ERROR, "HX711: reading saturated - check load cell bridge wiring");
        hx711_close(&m_hx);
        m_ready = false;
        return false;
    }

    m_ready = true;
    drain();
    return true;
}

void MassSensor::close()
{
    if (m_ready) {
        hx711_close(&m_hx);
        m_ready = false;
    }
}

bool MassSensor::poll_raw(int32_t &counts_out)
{
    if (!m_ready) {
        return false;
    }
    return hx711_get_value_noblock(&m_hx, &counts_out);
}

bool MassSensor::poll(double &grams_out)
{
    int32_t raw = 0;
    if (!poll_raw(raw)) {
        return false;
    }
    grams_out = counts_to_grams(raw - m_baseline);
    return true;
}

bool MassSensor::tare(uint16_t samples)
{
    if (!m_ready || samples == 0) {
        return false;
    }

    // Throw away anything already queued, so the average is built purely from
    // readings taken after the caller asked to tare.
    drain();

    // int64_t because 24-bit samples accumulated over hundreds of readings
    // would overflow a 32-bit sum.
    int64_t sum = 0;
    uint16_t collected = 0;

    const absolute_time_t deadline = make_timeout_time_ms(TARE_TIMEOUT_MS);

    while (collected < samples && !time_reached(deadline)) {
        int32_t raw = 0;
        if (poll_raw(raw)) {
            sum += raw;
            collected++;
        } else {
            tight_loop_contents();
        }
    }

    if (collected == 0) {
        log(LogLevel::ERROR, "HX711: tare failed - no samples received");
        return false;
    }

    if (collected < samples) {
        // Still usable, just averaged over fewer readings than intended, so
        // the baseline is a little noisier. Worth knowing about because it
        // usually means samples are arriving more slowly than the configured
        // rate suggests they should.
        log(LogLevel::WARNING, "HX711: tare used fewer samples than requested");
    }

    m_baseline = static_cast<int32_t>(sum / collected);

    // The baseline just moved, so any sample still sitting in the queue was
    // measured against the old one. Drop them.
    drain();
    return true;
}

void MassSensor::drain()
{
    int32_t discard = 0;
    while (poll_raw(discard)) {
        tight_loop_contents();
    }
}

double MassSensor::counts_to_grams(int32_t counts) const
{
    return static_cast<double>(counts) / LOADCELL_COUNTS_PER_GRAM;
}
