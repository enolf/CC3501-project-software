#pragma once

#include <stdint.h>

/// Tracks a set of 1-Wire-style sensors (identified by 64-bit ROM code) over
/// time and reports faults that a bare "how many did we find" count cannot:
/// which specific sensor stopped responding, or which specific sensor is
/// still present but its reads keep failing.
///
/// Deliberately independent of any particular driver type (it only deals in
/// arrays of ROM codes), so it can be exercised with synthetic data off
/// hardware.
///
/// The tracked set is adaptive and only ever grows while the object exists:
/// once a ROM code is seen it is remembered as "expected" from then on, so a
/// sensor going missing can be reported even though discovery alone cannot
/// tell "unplugged on purpose" apart from "the wire broke". There is no way
/// to permanently retire a ROM code short of destroying and recreating the
/// monitor (in practice: reset/power-cycle the board after swapping a
/// sensor, so the tracked set rebuilds cleanly from whatever is present at
/// boot).
class SensorHealthMonitor {
public:
    /// Consecutive read failures on a still-present sensor before it is
    /// reported as a read fault, rather than tolerated as one-off bus noise.
    static constexpr uint8_t READ_FAILURE_THRESHOLD = 3;

    /// Maximum distinct ROM codes tracked for the life of this object. Only
    /// relevant if sensors are hot-swapped repeatedly without a reset (each
    /// swap leaves a permanently "missing" entry behind); a fixed sensor set
    /// never approaches this.
    static constexpr uint8_t MAX_TRACKED_SENSORS = 16;

    /// reminderIntervalCycles: how many noteDiscovery()/noteReadResult()
    /// calls between repeated reminder logs for a fault that is still active
    /// (as opposed to logging every single call, which would flood the log).
    explicit SensorHealthMonitor(uint32_t reminderIntervalCycles);

    /// Call after every bus (re)scan, successful or not, with the ROM codes
    /// currently discovered and the caller's own cycle counter (used only to
    /// pace reminder logging -- this class keeps no notion of real time).
    /// Logs newly-found sensors, sensors that stopped responding, and
    /// sensors that came back after being missing.
    void noteDiscovery(const uint64_t *romCodes, uint8_t romCodeCount, uint32_t currentCycle);

    /// Call after each read attempt for one sensor. Tracks consecutive
    /// failures per ROM code and logs a read fault once READ_FAILURE_THRESHOLD
    /// is reached, plus periodic reminders and a recovery message.
    void noteReadResult(uint64_t romCode, bool success, uint32_t currentCycle);

private:
    struct SensorRecord {
        uint64_t romCode = 0;
        bool inUse = false;                    // slot occupied
        bool present = false;                  // responded on the last discovery
        uint32_t lastMissingReminderCycle = 0;
        uint8_t consecutiveReadFailures = 0;
        bool readFaulted = false;              // consecutiveReadFailures crossed the threshold
        uint32_t lastReadFaultReminderCycle = 0;
    };

    int findIndexByRom(uint64_t romCode) const;
    int allocateSlot(uint64_t romCode);

    SensorRecord records[MAX_TRACKED_SENSORS];
    uint32_t reminderIntervalCycles;
};

/// True if a reading is exactly the DS18B20's power-on-reset default value
/// (+85.0000 C -- datasheet Table 1, raw register value 0x0550). A sensor
/// that browns out mid-cycle and is read before it has actually converted
/// again will report this value, so it is a useful "this reading may not be
/// real" signal rather than a genuine ambient temperature.
bool isPowerOnResetTemperature(float degreesC);
