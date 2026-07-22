#include <stdio.h>
// #include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "WS2812.pio.h" // This header file gets produced during compilation from the WS2812.pio file
#include "drivers/logging/logging.h"

#include "peripherals/tft_display.h"

// Files for DS18B20 temperature sensor
#include "drivers/DS18B20/DS18B20.h"
#include "tasks/sensor_health.h"

// Board-specific configuration
#include "board.h"


// Code example for testing the TFT display.
// --------------------------------------------------------------

// int main()
// {
//     stdio_init_all();

//     Display::init();
//     Display::write_text("CAN YOU SEE ME?");

//     for (;;) {
//         Display::run();
//         sleep_ms(5);
//     }

//     return 0;
// }

// --------------------------------------------------------------



// Code example for testing the DS18B20 temperature sensor. 
// --------------------------------------------------------------

// How often to take a temperature reading in the test loop.
constexpr uint32_t MEASUREMENT_INTERVAL_MS = 5000;

// How many measurement cycles between bus re-scans, so a sensor plugged in
// after startup is picked up without a reset. 
constexpr uint32_t RESCAN_INTERVAL_CYCLES = 10;

// How many measurement cycles between repeated reminder logs for a fault
// (missing sensor / persistent read failures) that is still active. 
constexpr uint32_t FAULT_REMINDER_INTERVAL_CYCLES = 10;

// Log the index and 64-bit ROM code of every discovered sensor, so each
// physical sensor can be identified (warm one and watch which reading moves).
static void printDiscoveredSensors(const DS18B20 &sensors)
{
    char msg[64];
    for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
        uint64_t address = sensors.sensorAddress(i);
        snprintf(msg, sizeof(msg), "DS18B20 sensor %u: ROM %08lX%08lX",
                 i,
                 (unsigned long)(address >> 32),
                 (unsigned long)(address & 0xFFFFFFFFu));
        log(LogLevel::INFORMATION, msg);
    }
}

int main()
{
    stdio_init_all();

    // Discover all DS18B20 sensors on the 1-Wire bus
    DS18B20 sensors(DS18B20_BUS_PIN, DS18B20_PULLUP_FET_PIN, DS18B20_PULLUP_FET_ACTIVE_LOW);
    sensors.setParasitePower(DS18B20_PARASITE_POWER);

    // Tracks the sensor set over time so faults (a sensor going missing, or
    // a still-present sensor whose reads keep failing) can be reported by
    // ROM code rather than just noticing the sensor count changed. See
    // src/tasks/sensor_health.h for the full design rationale, including why
    // swapping a sensor needs a reset/power-cycle afterwards.
    SensorHealthMonitor health(FAULT_REMINDER_INTERVAL_CYCLES);

    if (sensors.init()) {
        printDiscoveredSensors(sensors);
    } else {
        log(LogLevel::ERROR, "DS18B20: no sensors found at startup");
    }

    // Seed the health monitor's baseline with whatever was found at startup.
    uint64_t romCodes[DS18B20::MAX_SENSORS];
    for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
        romCodes[i] = sensors.sensorAddress(i);
    }
    health.noteDiscovery(romCodes, sensors.sensorCount(), 0);

    char msg[96];
    uint32_t cycleCount = 0;
    for (;;) {
        cycleCount++;

        // Re-scan the bus if nothing has been found yet (every cycle, so a
        // sensor plugged in soon after boot appears quickly), or every
        // RESCAN_INTERVAL_CYCLES even with sensors already present, so a
        // newly plugged-in sensor is picked up without a reset.
        bool dueForRescan = (cycleCount % RESCAN_INTERVAL_CYCLES) == 0;
        if (sensors.sensorCount() == 0 || dueForRescan) {
            sensors.init();
            for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
                romCodes[i] = sensors.sensorAddress(i);
            }
            health.noteDiscovery(romCodes, sensors.sensorCount(), cycleCount);
        }

        if (sensors.sensorCount() == 0) {
            log(LogLevel::WARNING, "DS18B20: no sensors on the bus");
            sleep_ms(MEASUREMENT_INTERVAL_MS);
            continue;
        }

        bool converted = sensors.convertAndWait();
        if (!converted) {
            sleep_ms(MEASUREMENT_INTERVAL_MS);
            continue;
        }

        // Print the temperature from each sensor in turn
        for (uint8_t i = 0; i < sensors.sensorCount(); i++) {
            float degreesC;
            uint64_t rom = sensors.sensorAddress(i);
            bool success = sensors.readTemperature(i, degreesC);
            health.noteReadResult(rom, success, cycleCount);

            if (!success) {
                snprintf(msg, sizeof(msg), "DS18B20 sensor %u: read failed", i);
                log(LogLevel::ERROR, msg);
            } else if (isPowerOnResetTemperature(degreesC)) {
                snprintf(msg, sizeof(msg),
                         "DS18B20 sensor %u: %.4f C (suspicious: matches power-on-reset default)",
                         i, degreesC);
                log(LogLevel::WARNING, msg);
            } else {
                snprintf(msg, sizeof(msg), "DS18B20 sensor %u: %.4f C", i, degreesC);
                log(LogLevel::INFORMATION, msg);
            }
        }

        sleep_ms(MEASUREMENT_INTERVAL_MS);
    }

    return 0;
}

// --------------------------------------------------------------




// Code example for testing piicodev RFID board
// --------------------------------------------------------------



// --------------------------------------------------------------




//Code example for testing the HX711 load cell amplifier
// --------------------------------------------------------------



// --------------------------------------------------------------