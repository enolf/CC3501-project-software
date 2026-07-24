#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

/// Driver for DS18B20 1-Wire temperature sensors.
///
/// One instance of this class manages one 1-Wire bus (one GPIO pin), which
/// may have several DS18B20 sensors attached to it. Each sensor is identified
/// by its factory-programmed 64-bit ROM code; init() discovers every sensor
/// on the bus automatically, and sensors are then addressed by index
/// (0 .. sensorCount()-1).
///
/// Power modes (see "Powering the DS18B20" in the datasheet):
///  - External supply: VDD wired to 3V3. Conversion progress can be polled.
///  - Parasite power: VDD wired to GND; the sensor runs off the data line.
///    During a conversion the bus must be driven high hard (a "strong
///    pull-up") to supply enough current, so the bus cannot be used and
///    progress cannot be polled — the driver waits the full conversion time.
///    Select the mode with setParasitePower() before calling init().
///
/// Strong pull-up hardware (datasheet Figure 6): a P-channel FET from 3V3 to
/// the data line, its gate driven by a second GPIO given to the constructor.
/// The driver interlocks the two pins so they can never fight each other:
/// the data pin is always released before the FET turns on, and the FET is
/// always turned off before the data pin can pull the line low (which would
/// otherwise short 3V3 to ground through the FET and the RP2040 pin).
/// If no FET pin is provided (NO_STRONG_PULLUP_PIN), parasite mode falls
/// back to driving the data pin itself push-pull high.
///
/// Assumptions:
///  - The data line has an external 4.7 kOhm pull-up to 3V3.
///  - Sensors use the power-on default 12-bit resolution (0.0625 C steps,
///    conversion time up to 750 ms).
///
/// Typical use:
///   DS18B20 sensors(DS18B20_BUS_PIN);
///   sensors.init();
///   sensors.convertAndWait();               // blocking, ~750 ms
///   float t;
///   sensors.readTemperature(0, t);
///
/// Or non-blocking:
///   sensors.startConversion();
///   ... do other work ...
///   if (sensors.isConversionDone()) { sensors.readTemperature(0, t); }
class DS18B20 {
public:
    /// Maximum number of sensors the driver will track on one bus.
    static constexpr uint8_t MAX_SENSORS = 8;

    /// Size of a 1-Wire ROM code in bytes (64 bits).
    static constexpr uint8_t ROM_SIZE = 8;

    /// Pass as strongPullupPin when the board has no strong pull-up FET.
    static constexpr uint NO_STRONG_PULLUP_PIN = 0xFFFFFFFF;

    /// Create a driver for the 1-Wire bus on the given GPIO pin.
    /// strongPullupPin is the GPIO driving the gate of the strong pull-up
    /// FET (or NO_STRONG_PULLUP_PIN if not fitted); strongPullupActiveLow is
    /// true for a P-channel FET (gate low = FET on, the datasheet circuit).
    /// No hardware access happens until init() is called.
    explicit DS18B20(uint busPin,
                     uint strongPullupPin = NO_STRONG_PULLUP_PIN,
                     bool strongPullupActiveLow = true);

    /// Tell the driver how the sensors are powered (see the class comment).
    /// true = parasite power (VDD grounded), false = external supply
    /// (the default). Call before init(); init() asks the sensors how they
    /// are actually wired (Read Power Supply command) and logs a warning if
    /// it contradicts this setting.
    void setParasitePower(bool enabled);

    /// Configure the GPIO, check that at least one device responds on the
    /// bus, and discover the ROM code of every attached sensor.
    /// Returns true if at least one sensor was found. Safe to call again
    /// later to re-scan the bus (e.g. after plugging in a sensor).
    bool init();

    /// Number of sensors discovered by the last successful init().
    uint8_t sensorCount() const;

    /// The 64-bit ROM code of a discovered sensor (family code in the least
    /// significant byte). Returns 0 if the index is out of range.
    uint64_t sensorAddress(uint8_t index) const;

    /// Start a temperature conversion on ALL sensors simultaneously.
    /// Returns immediately; the conversion takes up to 750 ms.
    /// In parasite-power mode this also turns on the strong pull-up, so the
    /// bus must be left alone until isConversionDone() returns true.
    bool startConversion();

    /// True once the conversion started by startConversion() has finished.
    /// External supply: polls the sensors. Parasite power: checks whether
    /// the full conversion time has elapsed (polling is impossible while the
    /// strong pull-up holds the line), and releases the strong pull-up once
    /// it has.
    bool isConversionDone();

    /// Blocking convenience wrapper: startConversion() then wait (with a
    /// timeout) until the conversion completes. Returns false on failure.
    bool convertAndWait();

    /// Read the result of the last conversion from one sensor, in degrees
    /// Celsius. The scratchpad CRC is verified, so a true return means the
    /// data arrived intact. Call only after a conversion has completed.
    bool readTemperature(uint8_t index, float &degreesC);

private:
    // -- Bus line control (open-drain emulated by switching pin direction) --
    void busDriveLow();
    void busRelease();

    // -- Strong pull-up: actively drive the line high to power parasite-fed
    //    sensors during a conversion --
    void enableStrongPullup();
    void disableStrongPullup();

    // -- 1-Wire protocol primitives --
    bool resetAndCheckPresence();
    void writeBit(bool bit);
    bool readBit();
    void writeByte(uint8_t value);
    uint8_t readByte();

    /// Address one specific sensor (reset + Match ROM + its ROM code).
    bool selectSensor(uint8_t index);

    /// Run the 1-Wire Search ROM algorithm to enumerate every device.
    bool searchRoms();

    /// Ask the sensors how they are powered (Read Power Supply command) and
    /// warn if it does not match the configured parasitePower setting.
    void checkPowerSupplyConfig();

    /// Dallas/Maxim CRC-8 (polynomial 0x8C, reflected), used for both ROM
    /// codes and scratchpad data.
    static uint8_t crc8(const uint8_t *data, uint8_t len);

    uint pin;                                ///< GPIO pin of the 1-Wire bus
    uint pullupPin;                          ///< GPIO driving the strong pull-up FET gate
    bool pullupActiveLow;                    ///< FET gate level that turns the FET ON is low?
    uint8_t count;                           ///< number of discovered sensors
    bool parasitePower;                      ///< sensors powered from the data line?
    bool strongPullupActive;                 ///< line currently driven high for power?
    uint32_t conversionStartMs;              ///< when the last conversion began
    uint8_t roms[MAX_SENSORS][ROM_SIZE];     ///< discovered ROM codes
};
