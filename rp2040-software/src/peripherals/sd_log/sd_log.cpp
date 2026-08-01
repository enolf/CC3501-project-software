#include "peripherals/sd_log/sd_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "ff.h"
#include "drivers/logging/logging.h"

namespace sd_log {

namespace {

/// 8.3 name deliberately: long filename support is switched off in ffconf.h,
/// which is what lets the 1.9 MB ffunicode.c stay out of the build entirely.
constexpr const char *LOG_PATH = "LOG.TXT";

/// Longest single line, including the timestamp prefix and the newline.
constexpr size_t LINE_MAX = 160;

// FatFs keeps the mounted volume's state in this object and holds a pointer to
// it, so it must outlive every call — hence file scope rather than a local in
// init(). Losing this is a classic FatFs bug: the mount appears to succeed and
// then every later access reads through a dangling pointer.
FATFS volume;

bool mounted = false;
unsigned long total_bytes = 0;

/// Turn a FatFs result code into something readable in the serial log.
const char *result_name(FRESULT result)
{
    switch (result) {
        case FR_OK:                  return "ok";
        case FR_DISK_ERR:            return "disk error";
        case FR_NOT_READY:           return "card not ready";
        case FR_NO_FILE:             return "no such file";
        case FR_NO_PATH:             return "no such path";
        case FR_DENIED:              return "denied (card full?)";
        case FR_WRITE_PROTECTED:     return "write protected";
        case FR_INVALID_DRIVE:       return "invalid drive";
        case FR_NOT_ENABLED:         return "no work area";
        case FR_NO_FILESYSTEM:       return "no FAT filesystem";
        case FR_TIMEOUT:             return "timeout";
        default:                     return "failed";
    }
}

} // namespace

bool init()
{
    mounted = false;
    total_bytes = 0;

    // Mount option 1 means "mount now" rather than deferring until first
    // access, so a missing or unformatted card is reported here, at startup,
    // instead of surfacing later from an unrelated call.
    FRESULT result = f_mount(&volume, "", 1);
    if (result != FR_OK) {
        logf(LogLevel::WARNING, "sd_log: mount failed (%s) - logging to serial only",
             result_name(result));
        return false;
    }

    // Confirm the file can actually be opened for append before claiming the
    // log works. A card that mounts read-only, or is full, fails here.
    FIL file;
    result = f_open(&file, LOG_PATH, FA_WRITE | FA_OPEN_APPEND);
    if (result != FR_OK) {
        logf(LogLevel::WARNING, "sd_log: cannot open %s (%s)",
             LOG_PATH, result_name(result));
        f_unmount("");
        return false;
    }

    const FSIZE_t existing = f_size(&file);
    f_close(&file);

    mounted = true;
    logf(LogLevel::INFORMATION, "sd_log: %s ready (%lu bytes already on card)",
         LOG_PATH, (unsigned long)existing);
    return true;
}

bool write_line(const char *text)
{
    if (!mounted) {
        return false;
    }

    // Opened and closed per line rather than held open. Slower, but it means
    // the directory entry and file size are correct on the card after every
    // single line, so yanking the power or the card loses nothing that was
    // already reported as written.
    FIL file;
    FRESULT result = f_open(&file, LOG_PATH, FA_WRITE | FA_OPEN_APPEND);
    if (result != FR_OK) {
        logf(LogLevel::ERROR, "sd_log: open failed (%s)", result_name(result));
        return false;
    }

    // Timestamp is milliseconds since boot: the board has no RTC, so this is
    // the only clock it has. Anything correlating these with wall-clock time
    // does it on the Pi, which is the same scheme the serial protocol uses.
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    // f_printf is available because FF_USE_STRFUNC is 2 in ffconf.h, which also
    // makes it translate \n into \r\n so the file opens tidily in Notepad.
    const int written = f_printf(&file, "[%lu.%03lu] %s\n",
                                 (unsigned long)(now / 1000),
                                 (unsigned long)(now % 1000),
                                 text);

    // f_close flushes; without it the data sits in FatFs's buffer and the file
    // size on the card is never updated.
    result = f_close(&file);

    if (written < 0 || result != FR_OK) {
        logf(LogLevel::ERROR, "sd_log: write failed (%s)", result_name(result));
        return false;
    }

    total_bytes += static_cast<unsigned long>(written);
    return true;
}

bool write_linef(const char *fmt, ...)
{
    if (!mounted) {
        return false;
    }

    char buffer[LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    return write_line(buffer);
}

bool is_available()
{
    return mounted;
}

unsigned long bytes_written()
{
    return total_bytes;
}

} // namespace sd_log
