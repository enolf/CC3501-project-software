#include "peripherals/temperature_task/temperature_task.h"

#include "timings.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "board.h"
#include "drivers/ds18b20/ds18b20.h"
#include "drivers/logging/logging.h"
#include "peripherals/pi_link/pi_link.h"
#include "peripherals/sensor_health/sensor_health.h"

namespace temperature {

namespace {


/// How many sample cycles between re-scans of the 1-Wire bus, so a sensor
/// plugged in after boot is picked up without a reset.
constexpr uint32_t RESCAN_EVERY_CYCLES = 10;

/// The RP2040's internal temperature sensor is on ADC input 4.
constexpr uint ADC_TEMPERATURE_INPUT = 4;

/// Conversion constants from the RP2040 datasheet section 4.9.5: the sensor
/// reads 0.706 V at 27 C and falls by 1.721 mV per degree.
constexpr float ADC_REFERENCE_VOLTS = 3.3f;
constexpr float ADC_COUNTS = 4096.0f;
constexpr float DIE_VOLTS_AT_27C = 0.706f;
constexpr float DIE_VOLTS_PER_DEGREE = 0.001721f;

/// Where the task is up to. The whole point of this enum is that no step waits.
enum class Phase : uint8_t {
    Waiting,     ///< Counting down to the next sample
    Converting,  ///< Conversion started; polling for it to finish
    Reading,     ///< Conversion done; reading sensors one per pass
};

DS18B20 sensors(DS18B20_BUS_PIN, DS18B20_PULLUP_FET_PIN,
                DS18B20_PULLUP_FET_ACTIVE_LOW);
SensorHealthMonitor health(RESCAN_EVERY_CYCLES);

Phase    phase = Phase::Waiting;
uint32_t next_sample_ms = 0;
uint32_t cycle = 0;
uint8_t  reading_index = 0;
bool     ready = false;
float    die_temperature = 0.0f;

uint32_t now_ms()
{
    return to_ms_since_boot(get_absolute_time());
}

void read_die_temperature()
{
    adc_select_input(ADC_TEMPERATURE_INPUT);
    const float volts = (float)adc_read() * ADC_REFERENCE_VOLTS / ADC_COUNTS;
    die_temperature = 27.0f - (volts - DIE_VOLTS_AT_27C) / DIE_VOLTS_PER_DEGREE;
}

/// Re-scan the bus and tell the health monitor what is out there now.
void rescan()
{
    sensors.init();

    uint64_t roms[DS18B20::MAX_SENSORS];
    for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
        roms[i] = sensors.sensorAddress(i);
    }
    health.noteDiscovery(roms, sensors.sensorCount(), cycle);
}

} // namespace

bool init()
{
    // The RP2040's own sensor is free and needs only enabling.
    adc_init();
    adc_set_temp_sensor_enabled(true);
    read_die_temperature();

    sensors.setParasitePower(DS18B20_PARASITE_POWER);
    ready = sensors.init();

    if (!ready) {
        log(LogLevel::WARNING,
            "temperature: no DS18B20 found - the fridge still trades, blind");
    } else {
        // Log every ROM code once. Until the dashboard exists this is the only
        // way to find out which physical sensor is which: warm one and watch
        // which reading moves.
        for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
            const uint64_t rom = sensors.sensorAddress(i);
            logf(LogLevel::INFORMATION, "temperature: sensor %u rom %08lX%08lX",
                 i, (unsigned long)(rom >> 32),
                 (unsigned long)(rom & 0xFFFFFFFFu));
        }
    }

    uint64_t roms[DS18B20::MAX_SENSORS];
    for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
        roms[i] = sensors.sensorAddress(i);
    }
    health.noteDiscovery(roms, sensors.sensorCount(), 0);

    next_sample_ms = now_ms() + timings::TEMP_SAMPLE_INTERVAL_MS;
    phase = Phase::Waiting;
    return ready;
}

void run_temperature()
{
    switch (phase) {
        case Phase::Waiting: {
            if ((int32_t)(now_ms() - next_sample_ms) < 0) {
                return;
            }
            cycle++;
            read_die_temperature();

            // Re-scan periodically, and every cycle while nothing has been
            // found, so a sensor connected after boot appears without a reset.
            if (sensors.sensorCount() == 0 || (cycle % RESCAN_EVERY_CYCLES) == 0) {
                rescan();
            }

            if (sensors.sensorCount() == 0) {
                next_sample_ms = now_ms() + timings::TEMP_SAMPLE_INTERVAL_MS;
                return;
            }

            // Kick off the conversion and LEAVE. This is the call that used to
            // be convertAndWait(), which slept for 750 ms; the driver exposes
            // the two halves separately, so nothing here has to wait for it.
            if (!sensors.startConversion()) {
                log(LogLevel::WARNING, "temperature: could not start a conversion");
                next_sample_ms = now_ms() + timings::TEMP_SAMPLE_INTERVAL_MS;
                return;
            }
            phase = Phase::Converting;
            return;
        }

        case Phase::Converting: {
            if (!sensors.isConversionDone()) {
                return;     // still cooking; ~750 ms at 12-bit resolution
            }
            reading_index = 0;
            phase = Phase::Reading;
            return;
        }

        case Phase::Reading: {
            if (reading_index >= sensors.sensorCount()) {
                next_sample_ms = now_ms() + timings::TEMP_SAMPLE_INTERVAL_MS;
                phase = Phase::Waiting;
                return;
            }

            // ONE sensor per pass. Reading a scratchpad is about 6 ms of
            // bit-banged 1-Wire timing that cannot be interrupted, so three
            // sensors back to back would be a visible 18 ms hitch. Spread out,
            // each pass stays well inside the loop-time budget.
            const uint8_t index = reading_index++;
            const uint64_t rom = sensors.sensorAddress(index);

            float celsius = 0.0f;
            const bool ok = sensors.readTemperature(index, celsius);
            health.noteReadResult(rom, ok, cycle);

            if (!ok) {
                logf(LogLevel::WARNING, "temperature: sensor %u would not read", index);
                return;
            }

            if (isPowerOnResetTemperature(celsius)) {
                // 85 C exactly is the DS18B20's power-on default, so it usually
                // means the conversion did not actually happen rather than that
                // the fridge is on fire.
                logf(LogLevel::WARNING,
                     "temperature: sensor %u reads the power-on default, ignoring",
                     index);
                return;
            }

            pi_link::notify_temperature(rom, celsius);
            return;
        }
    }
}

uint8_t sensor_count()
{
    return sensors.sensorCount();
}

float die_celsius()
{
    return die_temperature;
}

} // namespace temperature
