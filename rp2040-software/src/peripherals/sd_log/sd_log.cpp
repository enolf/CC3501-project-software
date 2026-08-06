#include "peripherals/sd_log/sd_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"
#include "drivers/logging/logging.h"
#include "drivers/sd_card/sd_card.h"

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

/// The RAM copy of the log, so a card inserted late still gets the whole
/// session. 16 KB against the RP2040's 264 KB, which at ~60 characters a line
/// is a few hundred lines — far more than a demonstration produces.
constexpr size_t RING_BYTES = 16384;

char ring[RING_BYTES];
size_t ring_head = 0;           ///< where the next byte goes
size_t ring_used = 0;           ///< bytes currently held
unsigned long ring_lines = 0;   ///< whole lines currently held
unsigned long ring_dropped = 0; ///< lines discarded to make room, since boot

/// Oldest byte still held.
size_t ring_tail()
{
    return (ring_head + RING_BYTES - ring_used) % RING_BYTES;
}

/// Discard the oldest whole line, so the buffer never holds a partial one.
void ring_drop_oldest()
{
    size_t at = ring_tail();
    size_t dropped = 0;
    while (dropped < ring_used) {
        const char c = ring[at];
        at = (at + 1) % RING_BYTES;
        dropped++;
        if (c == '\n') {
            break;      // the newline belongs to the line being dropped
        }
    }
    ring_used -= dropped;
    if (ring_lines > 0) {
        ring_lines--;
    }
    ring_dropped++;
}

/// Append one line plus its newline. Always succeeds; something older goes if
/// there is no room.
void ring_push(const char *text)
{
    size_t length = strlen(text);
    // A line longer than the buffer would loop forever below, dropping every
    // line and still not fitting. Cannot happen with LINE_MAX at 160, but the
    // bound is cheap and the failure it prevents is a hang.
    if (length > RING_BYTES - 1) {
        length = RING_BYTES - 1;
    }

    while (ring_used + length + 1 > RING_BYTES) {
        ring_drop_oldest();
    }

    for (size_t i = 0; i < length; i++) {
        ring[ring_head] = text[i];
        ring_head = (ring_head + 1) % RING_BYTES;
    }
    ring[ring_head] = '\n';
    ring_head = (ring_head + 1) % RING_BYTES;

    ring_used += length + 1;
    ring_lines++;
}

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
    // Timestamp is milliseconds since boot: the board has no RTC, so this is
    // the only clock it has. Anything correlating these with wall-clock time
    // does it on the Pi, which is the same scheme the serial protocol uses.
    //
    // Stamped ONCE, here, and the same string goes to both the card and the
    // buffer. Stamping again at dump time would date every line to the moment
    // somebody pressed the button, which is the one time that is of no
    // interest whatsoever.
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    char stamped[LINE_MAX + 16];
    snprintf(stamped, sizeof(stamped), "[%lu.%03lu] %s",
             (unsigned long)(now / 1000), (unsigned long)(now % 1000), text);

    // BEFORE the mounted check, deliberately. With no card this is the only
    // record that survives, and it is what the panel button exists to recover.
    ring_push(stamped);

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

    // f_printf is available because FF_USE_STRFUNC is 2 in ffconf.h, which also
    // makes it translate \n into \r\n so the file opens tidily in Notepad.
    const int written = f_printf(&file, "%s\n", stamped);

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
    // No `mounted` check here any more: write_line() buffers before it looks at
    // the card, and skipping the format would leave a hole in the RAM log
    // exactly when the RAM log is the only one there is.
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

unsigned long buffered_lines()
{
    return ring_lines;
}

unsigned long dropped_lines()
{
    return ring_dropped;
}

bool dump_to_card()
{
    // Re-run the card's initialisation handshake, unconditionally.
    //
    // NOT redundant with the f_mount below. FatFs asks disk_initialize() to
    // bring the card up, and that function short-circuits when
    // sd_card_is_ready() is already true — a sensible optimisation that costs
    // nearly a second when it applies, and exactly the wrong behaviour here.
    // The card in the slot NOW may not be the card that was probed: it may have
    // been pushed in after boot, or pulled out, taken to a laptop and put back.
    // A reinserted card has powered down and forgotten its state, so talking to
    // it on the strength of a probe from before it was removed fails in ways
    // that look like a corrupt filesystem rather than an uninitialised card.
    //
    // Probing every time costs up to a second on a button press nobody is
    // timing, which is the cheapest part of this whole operation.
    SdCardInfo info;
    if (!sd_card_probe(info)) {
        log(LogLevel::WARNING, "sd_log: no card responded to the probe");
        mounted = false;
        return false;
    }

    // Mounted here rather than trusting init(): the card is expected to have
    // been pushed in seconds ago, long after boot, so whatever happened at
    // startup says nothing about what is in the slot now.
    FRESULT result = f_mount(&volume, "", 1);
    if (result != FR_OK) {
        logf(LogLevel::WARNING, "sd_log: dump mount failed (%s)",
             result_name(result));
        mounted = false;
        return false;
    }

    FIL file;
    result = f_open(&file, LOG_PATH, FA_WRITE | FA_OPEN_APPEND);
    if (result != FR_OK) {
        logf(LogLevel::WARNING, "sd_log: dump cannot open %s (%s)",
             LOG_PATH, result_name(result));
        f_unmount("");
        mounted = false;
        return false;
    }

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    bool ok = f_printf(&file, "---- DUMP BEGIN at %lu.%03lu ms, %lu lines, "
                              "%lu dropped ----\n",
                       (unsigned long)(now / 1000), (unsigned long)(now % 1000),
                       ring_lines, ring_dropped) >= 0;

    // Byte at a time through the ring rather than one big write, because the
    // buffer wraps: the oldest line usually starts near the end of the array
    // and continues at the start, so a single f_write of the whole thing would
    // put the file in the wrong order.
    size_t at = ring_tail();
    for (size_t i = 0; i < ring_used && ok; i++) {
        ok = f_putc(ring[at], &file) >= 0;
        at = (at + 1) % RING_BYTES;
    }

    if (ok) {
        ok = f_printf(&file, "---- DUMP END ----\n") >= 0;
    }

    // f_close flushes. Checked, not assumed: a card that fills up mid-dump
    // reports it here and nowhere earlier, and reporting success then would put
    // "remove SD card" on the screen over a truncated file.
    const FRESULT closed = f_close(&file);
    f_unmount("");

    // The card is about to be pulled out, so stop pretending it is writable.
    // Anything logged after this lives in RAM until the next dump.
    mounted = false;

    if (!ok || closed != FR_OK) {
        logf(LogLevel::WARNING, "sd_log: dump failed (%s)", result_name(closed));
        return false;
    }

    logf(LogLevel::INFORMATION, "sd_log: dumped %lu lines (%lu bytes) to %s",
         ring_lines, (unsigned long)ring_used, LOG_PATH);
    return true;
}

} // namespace sd_log
