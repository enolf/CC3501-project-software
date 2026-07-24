// Fault tracking for a set of ROM-addressed sensors. See sensor_health.h for
// the design rationale (adaptive tracked set, why a reset is needed to
// permanently retire a sensor).

#include <stddef.h>
#include <stdio.h>

#include "sensor_health.h"
#include "drivers/logging/logging.h"

/// Render a 64-bit ROM code as the same fixed-width hex pairing used
/// elsewhere in the project (high 32 bits, then low 32 bits), so log
/// messages here match the sensor listing printed at startup.
static void formatRom(uint64_t romCode, char *buf, size_t bufSize)
{
    snprintf(buf, bufSize, "%08lX%08lX",
             (unsigned long)(romCode >> 32),
             (unsigned long)(romCode & 0xFFFFFFFFu));
}

SensorHealthMonitor::SensorHealthMonitor(uint32_t reminderIntervalCycles)
    : reminderIntervalCycles(reminderIntervalCycles)
{
}

int SensorHealthMonitor::findIndexByRom(uint64_t romCode) const
{
    for (uint8_t i = 0; i < MAX_TRACKED_SENSORS; i++) {
        if (records[i].inUse && records[i].romCode == romCode) {
            return i;
        }
    }
    return -1;
}

int SensorHealthMonitor::allocateSlot(uint64_t romCode)
{
    for (uint8_t i = 0; i < MAX_TRACKED_SENSORS; i++) {
        if (!records[i].inUse) {
            records[i] = SensorRecord{};
            records[i].inUse = true;
            records[i].romCode = romCode;
            return i;
        }
    }
    return -1;
}

void SensorHealthMonitor::noteDiscovery(const uint64_t *romCodes, uint8_t romCodeCount, uint32_t currentCycle)
{
    // Track which known records were seen in this pass, so anything left
    // unmarked below can be recognised as missing.
    bool seen[MAX_TRACKED_SENSORS] = {false};
    char romStr[20];
    char msg[128];

    for (uint8_t i = 0; i < romCodeCount; i++) {
        uint64_t rom = romCodes[i];
        int idx = findIndexByRom(rom);

        if (idx < 0) {
            idx = allocateSlot(rom);
            if (idx < 0) {
                log(LogLevel::WARNING, "SensorHealth: tracking table full, cannot register a new sensor ROM");
                continue;
            }
            formatRom(rom, romStr, sizeof(romStr));
            snprintf(msg, sizeof(msg), "SensorHealth: new sensor ROM %s", romStr);
            log(LogLevel::INFORMATION, msg);
        } else if (!records[idx].present) {
            formatRom(rom, romStr, sizeof(romStr));
            snprintf(msg, sizeof(msg), "SensorHealth: sensor ROM %s back online (recovered)", romStr);
            log(LogLevel::INFORMATION, msg);
        }

        records[idx].present = true;
        seen[idx] = true;
    }

    // Anything tracked but not seen this pass either just went missing, or
    // was already missing and may be due for a reminder.
    for (uint8_t i = 0; i < MAX_TRACKED_SENSORS; i++) {
        if (!records[i].inUse || seen[i]) {
            continue;
        }

        if (records[i].present) {
            records[i].present = false;
            records[i].lastMissingReminderCycle = currentCycle;
            formatRom(records[i].romCode, romStr, sizeof(romStr));
            snprintf(msg, sizeof(msg), "SensorHealth: sensor ROM %s is no longer responding (missing)", romStr);
            log(LogLevel::ERROR, msg);
        } else if ((currentCycle - records[i].lastMissingReminderCycle) >= reminderIntervalCycles) {
            records[i].lastMissingReminderCycle = currentCycle;
            formatRom(records[i].romCode, romStr, sizeof(romStr));
            snprintf(msg, sizeof(msg), "SensorHealth: sensor ROM %s still missing", romStr);
            log(LogLevel::WARNING, msg);
        }
    }
}

void SensorHealthMonitor::noteReadResult(uint64_t romCode, bool success, uint32_t currentCycle)
{
    int idx = findIndexByRom(romCode);
    if (idx < 0) {
        // noteDiscovery() always registers a ROM code before it can be read,
        // so this should not happen; nothing sensible to track otherwise.
        return;
    }
    SensorRecord &rec = records[idx];
    char romStr[20];
    char msg[128];

    if (success) {
        if (rec.readFaulted) {
            formatRom(romCode, romStr, sizeof(romStr));
            snprintf(msg, sizeof(msg), "SensorHealth: sensor ROM %s reads recovered", romStr);
            log(LogLevel::INFORMATION, msg);
        }
        rec.consecutiveReadFailures = 0;
        rec.readFaulted = false;
        return;
    }

    if (rec.consecutiveReadFailures < UINT8_MAX) {
        rec.consecutiveReadFailures++;
    }

    if (!rec.readFaulted && rec.consecutiveReadFailures >= READ_FAILURE_THRESHOLD) {
        rec.readFaulted = true;
        rec.lastReadFaultReminderCycle = currentCycle;
        formatRom(romCode, romStr, sizeof(romStr));
        snprintf(msg, sizeof(msg),
                 "SensorHealth: sensor ROM %s has %u consecutive read failures (check wiring/sensor)",
                 romStr, rec.consecutiveReadFailures);
        log(LogLevel::ERROR, msg);
    } else if (rec.readFaulted && (currentCycle - rec.lastReadFaultReminderCycle) >= reminderIntervalCycles) {
        rec.lastReadFaultReminderCycle = currentCycle;
        formatRom(romCode, romStr, sizeof(romStr));
        snprintf(msg, sizeof(msg), "SensorHealth: sensor ROM %s read failures persisting", romStr);
        log(LogLevel::WARNING, msg);
    }
}

bool isPowerOnResetTemperature(float degreesC)
{
    // +85.0000 C is exactly representable in a float (raw register value
    // 1360, LSB = 1/16 C = 2^-4), so a direct equality compare is safe here
    // -- there is no accumulated floating-point error to allow for.
    return degreesC == 85.0f;
}
