#pragma once

#include <stdbool.h>

// Appends text lines to LOG.TXT in the root of the SD card, so a log can be
// read by pulling the card and opening it on a laptop.
//
// SCOPE: a demonstration that the board can write storage a PC can read. It is
// not the event buffer described in plan.md Stage 14 — that needs the state
// machine, so that writes can be confined to Idle.
//
// EVERY CALL HERE BLOCKS. A card doing an internal erase can hold the bus for
// a couple of hundred milliseconds, and the file is flushed on every line so
// that pulling the power never loses more than the last one. That trade is
// right for a log whose whole purpose is surviving a crash, and wrong for
// anything in a time-critical path. Until the state machine exists, calls are
// made only at startup and from a debug key.

namespace sd_log {

/// Mount the card and open the log.
///
/// Appends to LOG.TXT if it exists and creates it otherwise, so history is kept
/// across power cycles. Returns false if there is no card, it is not FAT
/// formatted, or the file cannot be opened; the rest of the system runs
/// normally either way, because a missing log card must never stop the fridge
/// trading.
bool init();

/// Append one line. A newline is added; do not include one.
/// Returns false if the write failed or the log was never mounted.
bool write_line(const char *text);

/// Append a printf-formatted line.
bool write_linef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// True if the log is mounted and writable.
bool is_available();

/// Bytes written to the log file since it was opened.
unsigned long bytes_written();

} // namespace sd_log
