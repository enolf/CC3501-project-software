// Logging system, using the style that state is global in the C file.

#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "logging.h"

// --- Device driver internal state:

/// Drop messages whose level is below this threshold.
static LogLevel maxLogLevel = LogLevel::INFORMATION;

// --- Device driver functions
void setLogLevel(LogLevel newLevel)
{
    maxLogLevel = newLevel;
}

void log(LogLevel level, const char *msg)
{
    // Should we show this message?
    if (level < maxLogLevel) {
        return;
    }

    // Get the time since boot
    uint32_t time = to_ms_since_boot(get_absolute_time());
    uint32_t time_sec = time / 1000;
    uint32_t time_decimal = (time % 1000);

    // Convert the level to a string.
    //
    // Initialised rather than merely assigned in every case: the switch covers
    // every enumerator, but nothing stops a caller passing a value outside the
    // enum, and the compiler is right to point out that levelStr would then be
    // used uninitialised — printf would dereference a garbage pointer and take
    // the system down from a logging call.
    const char *levelStr = "Unknown";
    switch (level) {
        case LogLevel::INFORMATION:
            levelStr = "Information";
            break;
        case LogLevel::WARNING:
            levelStr = "Warning";
            break;
        case LogLevel::ERROR:
            levelStr = "Error";
            break;
    }

    // PRIu32 rather than %u: uint32_t is `long unsigned int` on this toolchain,
    // so a bare %u is a type mismatch. It happens to work because both are 32
    // bits here, but the compiler flags it and it would break on a target where
    // they differ.
    printf("[%" PRIu32 ".%03" PRIu32 " %s]: %s\n", time_sec, time_decimal, levelStr, msg);
}

void logf(LogLevel level, const char *fmt, ...)
{
    // Check the threshold before doing any formatting work. Building a string
    // that is then thrown away is pure waste, and the whole point of having a
    // level threshold is to make suppressed logging cheap.
    if (level < maxLogLevel) {
        return;
    }

    // File-scope rather than a local, so that a 128-byte frame is not pushed
    // onto the stack at every logging site. That is only safe because this
    // project is a single-core superloop with no interrupt handler ever calling
    // in: two callers sharing one buffer would otherwise interleave and produce
    // garbage. If logging from an ISR is ever needed, this must become a local.
    static char buffer[LOG_MESSAGE_MAX_LEN];

    va_list args;
    va_start(args, fmt);
    // vsnprintf always null-terminates within the size given, so an oversized
    // message is truncated rather than overflowing the buffer.
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    log(level, buffer);
}
