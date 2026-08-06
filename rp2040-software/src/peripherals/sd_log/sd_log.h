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

// --- On-demand export -------------------------------------------------------
//
// Every line is ALSO kept in a RAM ring buffer, whether or not a card is
// mounted. That is what makes the panel button useful: the card can be absent
// for the whole session and inserted only to collect the log, which is how
// somebody actually uses this. Write-through to a card present at boot is
// unchanged, so the crash-survival property above still holds.
//
// When the buffer fills, the OLDEST WHOLE LINES are dropped rather than the
// newest. A log that stops recording once it is full is a log that misses the
// thing you went looking for, and dropping part of a line would produce a file
// with a corrupt row in the middle of it.

/// Mount whatever card is in the slot now and append the whole buffer to it.
///
/// BLOCKS, for as long as it takes to write every buffered line — hundreds of
/// milliseconds is normal and a slow card can take seconds. Call it only from
/// a state where nothing is waiting on the superloop; `checkout` confines it
/// to Idle for exactly that reason.
///
/// Mounts here rather than relying on `init()` because the card is expected to
/// have been inserted long after boot. Unmounts afterwards, so the card is safe
/// to pull the moment the screen says so — which also means write-through stops
/// until the next `init()`, since the card is on its way out of the slot.
///
/// The buffer is NOT cleared. A dump can therefore be repeated onto a second
/// card, and a failed one loses nothing; the cost is that dumping twice onto
/// the same card writes the lines twice, which the BEGIN/END markers make
/// visible rather than mysterious.
bool dump_to_card();

/// How many whole lines are waiting in the RAM buffer.
unsigned long buffered_lines();

/// How many lines have been discarded to make room since boot. Non-zero means
/// the buffer wrapped and the oldest history is gone.
unsigned long dropped_lines();

} // namespace sd_log
