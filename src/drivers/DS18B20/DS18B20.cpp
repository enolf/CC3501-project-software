// DS18B20 1-Wire temperature sensor driver.
//
// The 1-Wire bus is bit-banged on a single GPIO. The line is open-drain:
// we never drive it high. Instead, "low" = pin set as output (pre-loaded
// with 0), and "released" = pin set as input, letting the pull-up resistor
// raise the line. The sensors pull the line low themselves to answer.
//
// Timing is the whole protocol: each bit is a precisely-timed pulse ("time
// slot"). Interrupts are disabled for the duration of each slot so that an
// interrupt handler cannot stretch a pulse and corrupt the bit. Slots are at
// most ~70 us, so interrupts are never off for long.

#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "DS18B20.h"
#include "drivers/logging/logging.h"

// --- 1-Wire ROM commands (address a device on the bus) ---
static constexpr uint8_t CMD_SEARCH_ROM      = 0xF0; // enumerate all ROM codes
static constexpr uint8_t CMD_MATCH_ROM       = 0x55; // select one device by ROM code
static constexpr uint8_t CMD_SKIP_ROM        = 0xCC; // address every device at once

// --- DS18B20 function commands ---
static constexpr uint8_t CMD_CONVERT_T         = 0x44; // start temperature conversion
static constexpr uint8_t CMD_READ_SCRATCHPAD   = 0xBE; // read the 9-byte scratchpad
static constexpr uint8_t CMD_READ_POWER_SUPPLY = 0xB4; // ask sensors how they are powered

/// DS18B20 devices have family code 0x28 in the first ROM byte.
static constexpr uint8_t DS18B20_FAMILY_CODE = 0x28;

/// The scratchpad is 9 bytes: temperature LSB/MSB, alarm registers, config,
/// reserved bytes, and a CRC of the first 8 bytes.
static constexpr uint8_t SCRATCHPAD_SIZE = 9;
static constexpr uint8_t SCRATCHPAD_TEMP_LSB = 0;
static constexpr uint8_t SCRATCHPAD_TEMP_MSB = 1;
static constexpr uint8_t SCRATCHPAD_CRC = 8;

/// Temperature register is a signed 16-bit value in units of 1/16 C.
static constexpr float TEMP_LSB_DEGREES = 1.0f / 16.0f;

// --- Standard-speed 1-Wire timing, in microseconds.
// Values follow the recommended timings in Maxim application note 126
// ("1-Wire Communication Through Software").
static constexpr uint32_t T_RESET_LOW_US     = 480; // reset pulse (spec: >= 480)
static constexpr uint32_t T_PRESENCE_WAIT_US = 70;  // release-to-sample for presence
static constexpr uint32_t T_RESET_TAIL_US    = 410; // rest of the presence window
static constexpr uint32_t T_WRITE1_LOW_US    = 6;   // write-1: brief low pulse
static constexpr uint32_t T_WRITE1_HIGH_US   = 64;  // write-1: rest of the slot
static constexpr uint32_t T_WRITE0_LOW_US    = 60;  // write-0: hold low most of the slot
static constexpr uint32_t T_WRITE0_HIGH_US   = 10;  // write-0: recovery time
static constexpr uint32_t T_READ_LOW_US      = 6;   // read: brief low pulse to start slot
static constexpr uint32_t T_READ_SAMPLE_US   = 9;   // read: release-to-sample delay
static constexpr uint32_t T_READ_TAIL_US     = 55;  // read: rest of the slot

/// A 12-bit conversion takes at most 750 ms (datasheet t_CONV). In parasite
/// power mode the driver simply waits this long, since progress cannot be
/// polled while the strong pull-up holds the line high.
static constexpr uint32_t CONVERSION_TIME_MS = 750;
/// Timeout for the blocking wait; a little margin on top of t_CONV.
static constexpr uint32_t CONVERSION_TIMEOUT_MS = 800;
static constexpr uint32_t CONVERSION_POLL_MS = 10;

// --- Small helpers for the Search ROM algorithm (ROM bits are sent LSB first,
// so bit N of the 64-bit code lives at byte N/8, bit N%8).
static bool getRomBit(const uint8_t *rom, int bitIndex)
{
    return (rom[bitIndex / 8] >> (bitIndex % 8)) & 0x01;
}

static void setRomBit(uint8_t *rom, int bitIndex, bool value)
{
    if (value) {
        rom[bitIndex / 8] |= (uint8_t)(1u << (bitIndex % 8));
    } else {
        rom[bitIndex / 8] &= (uint8_t)~(1u << (bitIndex % 8));
    }
}

DS18B20::DS18B20(uint busPin, uint strongPullupPin, bool strongPullupActiveLow)
    : pin(busPin), pullupPin(strongPullupPin), pullupActiveLow(strongPullupActiveLow),
      count(0), parasitePower(false), strongPullupActive(false), conversionStartMs(0)
{
    memset(roms, 0, sizeof(roms));
}

void DS18B20::setParasitePower(bool enabled)
{
    parasitePower = enabled;
}

bool DS18B20::init()
{
    // Configure the strong pull-up FET gate FIRST and force the FET off,
    // regardless of power mode: a floating gate could turn a P-channel FET
    // (partially) on, which would hold the bus high and block all comms.
    // The latch is written before the direction so the pin never drives the
    // gate at the wrong level, even for an instant.
    if (pullupPin != NO_STRONG_PULLUP_PIN) {
        gpio_init(pullupPin);
        gpio_put(pullupPin, pullupActiveLow ? 1 : 0); // FET off
        gpio_set_dir(pullupPin, GPIO_OUT);
    }
    strongPullupActive = false;

    gpio_init(pin);
    // Pre-load the output latch with 0. From now on we emulate open-drain:
    // switching the pin to output drives the line low, switching it to input
    // releases it.
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_IN);
    // Weak internal pull-up as a backup only — the external 4.7k resistor is
    // still required for reliable operation.
    gpio_pull_up(pin);

    count = 0;

    if (!resetAndCheckPresence()) {
        log(LogLevel::ERROR, "DS18B20: no presence pulse (check wiring and pull-up)");
        return false;
    }

    if (!searchRoms()) {
        return false;
    }
    if (count == 0) {
        return false;
    }

    // Sanity check: ask the sensors how they are wired and warn if it
    // contradicts the configured power mode (a mismatch causes bad readings
    // or a dragged-down bus, and is painful to debug from symptoms alone).
    checkPowerSupplyConfig();

    return true;
}

uint8_t DS18B20::sensorCount() const
{
    return count;
}

uint64_t DS18B20::sensorAddress(uint8_t index) const
{
    if (index >= count) {
        return 0;
    }
    // Assemble the 64-bit code with the family code (rom[0]) as the least
    // significant byte, matching the usual printed representation.
    uint64_t address = 0;
    for (int i = ROM_SIZE - 1; i >= 0; i--) {
        address = (address << 8) | roms[index][i];
    }
    return address;
}

bool DS18B20::startConversion()
{
    if (!resetAndCheckPresence()) {
        log(LogLevel::ERROR, "DS18B20: bus reset failed before conversion");
        return false;
    }
    // Skip ROM addresses every sensor at once, so one Convert T command
    // starts all of them converting in parallel.
    writeByte(CMD_SKIP_ROM);
    writeByte(CMD_CONVERT_T);
    conversionStartMs = to_ms_since_boot(get_absolute_time());

    if (parasitePower) {
        // Parasite-fed sensors draw up to 1.5 mA while converting — far more
        // than the pull-up resistor can supply — so drive the line high hard.
        // The datasheet requires this within 10 us of the Convert T command
        // (t_SPON); writeByte() ends with only the 10 us write-0 recovery
        // time, so we are right on time here.
        enableStrongPullup();
    }
    return true;
}

bool DS18B20::isConversionDone()
{
    if (parasitePower) {
        // The strong pull-up owns the bus, so the sensors cannot be polled;
        // the datasheet says to wait out the full conversion time instead.
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - conversionStartMs;
        if (elapsed < CONVERSION_TIME_MS) {
            return false;
        }
        disableStrongPullup();
        return true;
    }

    // With external power, a converting sensor answers read slots with 0 and
    // answers 1 once the conversion is complete.
    return readBit();
}

bool DS18B20::convertAndWait()
{
    if (!startConversion()) {
        return false;
    }
    for (uint32_t waited = 0; waited <= CONVERSION_TIMEOUT_MS; waited += CONVERSION_POLL_MS) {
        if (isConversionDone()) {
            return true;
        }
        sleep_ms(CONVERSION_POLL_MS);
    }
    log(LogLevel::ERROR, "DS18B20: conversion timed out");
    return false;
}

bool DS18B20::readTemperature(uint8_t index, float &degreesC)
{
    if (index >= count) {
        log(LogLevel::ERROR, "DS18B20: sensor index out of range");
        return false;
    }

    if (!selectSensor(index)) {
        return false;
    }
    writeByte(CMD_READ_SCRATCHPAD);

    uint8_t scratchpad[SCRATCHPAD_SIZE];
    for (uint8_t i = 0; i < SCRATCHPAD_SIZE; i++) {
        scratchpad[i] = readByte();
    }

    // The last scratchpad byte is a CRC of the first 8; if it does not match,
    // the data was corrupted on the wire (or the sensor was disconnected).
    if (crc8(scratchpad, SCRATCHPAD_SIZE - 1) != scratchpad[SCRATCHPAD_CRC]) {
        log(LogLevel::ERROR, "DS18B20: scratchpad CRC mismatch");
        return false;
    }

    // Temperature is a signed 16-bit value in 1/16ths of a degree C.
    // Assembling it into an int16_t makes negative temperatures work via
    // ordinary two's-complement sign extension.
    int16_t raw = (int16_t)(((uint16_t)scratchpad[SCRATCHPAD_TEMP_MSB] << 8)
                            | scratchpad[SCRATCHPAD_TEMP_LSB]);
    degreesC = raw * TEMP_LSB_DEGREES;
    return true;
}

// --------------------------------------------------------------------------
// Bus line control
// --------------------------------------------------------------------------

void DS18B20::busDriveLow()
{
    // Interlock: never pull the line low while the strong pull-up FET is
    // driving it high — that would short 3V3 to ground through the FET and
    // this pin. Guarding here, in the one primitive that pulls the line low,
    // makes cross-conduction impossible from any code path.
    if (strongPullupActive) {
        disableStrongPullup();
    }
    gpio_set_dir(pin, GPIO_OUT); // output latch is 0, so this pulls the line low
}

void DS18B20::busRelease()
{
    gpio_set_dir(pin, GPIO_IN); // pull-up raises the line (unless a sensor holds it low)
}

void DS18B20::enableStrongPullup()
{
    // Interlock: release the data pin BEFORE anything drives the bus high,
    // so the FET (or push-pull data pin) never fights our own low driver.
    busRelease();

    if (pullupPin != NO_STRONG_PULLUP_PIN) {
        // Turn on the strong pull-up FET (datasheet Figure 6 circuit).
        gpio_put(pullupPin, pullupActiveLow ? 0 : 1);
    } else {
        // No FET fitted: drive the data pin itself high push-pull. Nothing
        // else may use the bus until disableStrongPullup().
        gpio_put(pin, 1);
        gpio_set_dir(pin, GPIO_OUT);
    }
    strongPullupActive = true;
}

void DS18B20::disableStrongPullup()
{
    if (pullupPin != NO_STRONG_PULLUP_PIN) {
        // Turn the FET off; the data pin is already an input, so the bus
        // simply returns to the 4.7k resistor pull-up.
        gpio_put(pullupPin, pullupActiveLow ? 1 : 0);
    } else {
        gpio_set_dir(pin, GPIO_IN);
        gpio_put(pin, 0); // restore the low output latch that busDriveLow() relies on
    }
    strongPullupActive = false;
}

// --------------------------------------------------------------------------
// 1-Wire protocol primitives
// --------------------------------------------------------------------------

bool DS18B20::resetAndCheckPresence()
{
    // Every transaction starts with a reset, so this is the safety net that
    // guarantees the strong pull-up is off before any other bus activity.
    if (strongPullupActive) {
        disableStrongPullup();
    }

    // Hold the line low for >= 480 us to reset every device on the bus.
    // (An interrupt here only lengthens the pulse, which is harmless, so
    // interrupts stay enabled for this part.)
    busDriveLow();
    busy_wait_us_32(T_RESET_LOW_US);

    // Release the line, then sample: any sensor present pulls the line low
    // (the "presence pulse") 15-60 us after the release. This window is
    // timing-critical, so interrupts are off while we wait and sample.
    uint32_t irqState = save_and_disable_interrupts();
    busRelease();
    busy_wait_us_32(T_PRESENCE_WAIT_US);
    bool present = !gpio_get(pin); // line held low = at least one device present
    restore_interrupts(irqState);

    // Wait out the rest of the presence window before any commands.
    busy_wait_us_32(T_RESET_TAIL_US);
    return present;
}

void DS18B20::writeBit(bool bit)
{
    // Both bit values use a 70 us slot that starts with a falling edge.
    // A "1" is a short low pulse (the sensor samples the line high later in
    // the slot); a "0" holds the line low for most of the slot.
    uint32_t irqState = save_and_disable_interrupts();
    busDriveLow();
    busy_wait_us_32(bit ? T_WRITE1_LOW_US : T_WRITE0_LOW_US);
    busRelease();
    restore_interrupts(irqState);
    // Remainder of the slot / recovery time; not timing-critical.
    busy_wait_us_32(bit ? T_WRITE1_HIGH_US : T_WRITE0_HIGH_US);
}

bool DS18B20::readBit()
{
    // A read slot: we issue a short low pulse, release, and the sensor either
    // holds the line low (a 0) or lets it rise (a 1). The line must be
    // sampled within 15 us of the falling edge, so this is the most
    // timing-sensitive operation in the driver.
    uint32_t irqState = save_and_disable_interrupts();
    busDriveLow();
    busy_wait_us_32(T_READ_LOW_US);
    busRelease();
    busy_wait_us_32(T_READ_SAMPLE_US);
    bool bit = gpio_get(pin);
    restore_interrupts(irqState);
    busy_wait_us_32(T_READ_TAIL_US);
    return bit;
}

void DS18B20::writeByte(uint8_t value)
{
    // 1-Wire sends least significant bit first.
    for (uint8_t i = 0; i < 8; i++) {
        writeBit((value >> i) & 0x01);
    }
}

uint8_t DS18B20::readByte()
{
    uint8_t value = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (readBit()) {
            value |= (uint8_t)(1u << i);
        }
    }
    return value;
}

bool DS18B20::selectSensor(uint8_t index)
{
    if (!resetAndCheckPresence()) {
        log(LogLevel::ERROR, "DS18B20: bus reset failed while selecting sensor");
        return false;
    }
    // Match ROM followed by the 64-bit code makes only that one sensor listen
    // to the next command; all others ignore the bus until the next reset.
    writeByte(CMD_MATCH_ROM);
    for (uint8_t i = 0; i < ROM_SIZE; i++) {
        writeByte(roms[index][i]);
    }
    return true;
}

// --------------------------------------------------------------------------
// Search ROM: enumerate every device on the bus.
//
// The algorithm (from the Maxim 1-Wire documentation) walks the 64-bit ROM
// codes as a binary tree. For each bit position, every participating sensor
// sends its bit and then its complement:
//   bit=0, cmp=1  -> all remaining devices have a 0 here
//   bit=1, cmp=0  -> all remaining devices have a 1 here
//   bit=0, cmp=0  -> a "discrepancy": some have 0, some have 1 (a fork)
//   bit=1, cmp=1  -> nobody answered (error)
// The master then writes back the chosen bit, and only sensors matching that
// choice stay in the search. One full pass discovers one device; the search
// repeats, taking the other branch at the deepest unexplored fork, until
// every branch has been visited.
// --------------------------------------------------------------------------

bool DS18B20::searchRoms()
{
    uint8_t rom[ROM_SIZE] = {0};
    int lastDiscrepancy = -1;   // bit index of the deepest fork not yet fully explored
    bool lastDevice = false;

    while (!lastDevice) {
        if (count >= MAX_SENSORS) {
            log(LogLevel::WARNING, "DS18B20: more sensors on bus than MAX_SENSORS; some ignored");
            break;
        }

        if (!resetAndCheckPresence()) {
            log(LogLevel::ERROR, "DS18B20: bus reset failed during ROM search");
            return false;
        }
        writeByte(CMD_SEARCH_ROM);

        int discrepancyMarker = -1;
        for (int bitIndex = 0; bitIndex < 64; bitIndex++) {
            bool bit = readBit();
            bool complement = readBit();
            bool chosen;

            if (bit && complement) {
                // No device responded — sensor removed mid-search or bus fault.
                log(LogLevel::ERROR, "DS18B20: ROM search got no response");
                return false;
            } else if (bit != complement) {
                chosen = bit; // all remaining devices agree on this bit
            } else {
                // Fork: devices disagree at this bit position.
                if (bitIndex < lastDiscrepancy) {
                    chosen = getRomBit(rom, bitIndex); // retrace last pass's path
                } else if (bitIndex == lastDiscrepancy) {
                    chosen = true; // explored 0 last pass; take the 1 branch now
                } else {
                    chosen = false; // new fork: take the 0 branch first
                }
                if (!chosen) {
                    discrepancyMarker = bitIndex; // deepest fork where 1 is still unexplored
                }
            }

            setRomBit(rom, bitIndex, chosen);
            writeBit(chosen); // deselect every device whose bit differs
        }

        lastDiscrepancy = discrepancyMarker;
        if (lastDiscrepancy == -1) {
            lastDevice = true; // no unexplored forks remain
        }

        // Validate the discovered code before trusting it.
        if (crc8(rom, ROM_SIZE - 1) != rom[ROM_SIZE - 1]) {
            log(LogLevel::ERROR, "DS18B20: ROM search CRC mismatch");
            return false;
        }
        // An all-zero code passes CRC but indicates a bus fault (data line
        // shorted low), so reject it explicitly.
        uint8_t orAll = 0;
        for (uint8_t i = 0; i < ROM_SIZE; i++) {
            orAll |= rom[i];
        }
        if (orAll == 0) {
            log(LogLevel::ERROR, "DS18B20: ROM search returned all zeroes (data line shorted?)");
            return false;
        }
        if (rom[0] != DS18B20_FAMILY_CODE) {
            log(LogLevel::WARNING, "DS18B20: found a 1-Wire device that is not a DS18B20");
        }

        memcpy(roms[count], rom, ROM_SIZE);
        count++;
    }

    return true;
}

// Read Power Supply [B4h]: after this command, parasite-powered sensors pull
// the next read slot low while externally powered ones let the line stay
// high. So one read bit tells us whether ANY sensor on the bus is running on
// parasite power.
void DS18B20::checkPowerSupplyConfig()
{
    if (!resetAndCheckPresence()) {
        return; // bus errors are reported by the caller's next transaction
    }
    writeByte(CMD_SKIP_ROM);
    writeByte(CMD_READ_POWER_SUPPLY);
    bool anyParasite = !readBit();

    if (anyParasite && !parasitePower) {
        log(LogLevel::WARNING,
            "DS18B20: sensor(s) report parasite power but driver is set to external supply");
    } else if (!anyParasite && parasitePower) {
        log(LogLevel::WARNING,
            "DS18B20: driver is set to parasite power but all sensors report external supply");
    }
}

// Dallas/Maxim CRC-8: polynomial x^8 + x^5 + x^4 + 1, computed LSB-first
// (which is why the code shifts right and XORs with the reflected polynomial
// 0x8C). This matches the CRC the sensor appends to ROM codes and scratchpad
// reads.
uint8_t DS18B20::crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}
