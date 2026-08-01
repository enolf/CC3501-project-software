#pragma once 

/// Represents the priority of a log message.
enum LogLevel {
    INFORMATION,
    WARNING,
    ERROR,
};

/// Set the log level. Messages with a level below this threshold will be discarded.
void setLogLevel(LogLevel newLevel);

/// Log a new message.
void log(LogLevel level, const char *msg);

/// Log a printf-style formatted message.
///
/// log() takes a finished string, so every caller wanting a number in its
/// message had to declare its own buffer and snprintf into it first. This does
/// that once, here.
///
/// The format attribute makes the compiler check the format string against the
/// arguments exactly as it does for printf, so a %d handed a float is a build
/// error rather than a corrupted log line at three in the morning. (Argument 2
/// is the format, and the variadic arguments start at 3.)
///
/// Messages longer than LOG_MESSAGE_MAX_LEN are truncated, never overflowed.
/// NOT safe to call from an interrupt handler — see the note in logging.cpp.
void logf(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/// Longest formatted message, including the terminating null.
constexpr unsigned LOG_MESSAGE_MAX_LEN = 128;
