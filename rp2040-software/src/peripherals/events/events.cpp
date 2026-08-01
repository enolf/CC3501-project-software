// Event queue. State is file-scope, following the course convention for a
// one-of-a-kind module (CLAUDE.md section 4): there is exactly one event queue
// in the system, so there is nothing to gain from making it a class.

#include "peripherals/events/events.h"
#include "drivers/logging/logging.h"

namespace events {

namespace {

// A classic ring buffer. `head` is the next slot to read, `tail` the next slot
// to write, and `count` distinguishes the completely-full case from the
// completely-empty one (both of which otherwise look like head == tail).
Event    queue[QUEUE_CAPACITY];
uint8_t  head = 0;
uint8_t  tail = 0;
uint8_t  queued = 0;
uint32_t dropped_count = 0;

// Whether the "queue is full" error has already been logged. The condition
// repeats on every subsequent push until the loop catches up, and a log line
// per dropped event would bury the far more useful first one.
bool overflow_reported = false;

} // namespace

bool push(const Event &event)
{
    if (queued >= QUEUE_CAPACITY) {
        dropped_count++;
        if (!overflow_reported) {
            logf(LogLevel::ERROR,
                 "events: queue full (%u), dropping %s - a task is running long",
                 QUEUE_CAPACITY, name(event.kind));
            overflow_reported = true;
        }
        return false;
    }

    queue[tail] = event;
    // Wrap by comparison rather than %: QUEUE_CAPACITY need not be a power of
    // two, and the Cortex-M0+ has no hardware divide instruction.
    tail = (uint8_t)(tail + 1);
    if (tail >= QUEUE_CAPACITY) {
        tail = 0;
    }
    queued++;
    return true;
}

bool push(Kind kind)
{
    return push(Event(kind));
}

bool pop(Event &out)
{
    if (queued == 0) {
        return false;
    }

    out = queue[head];
    head = (uint8_t)(head + 1);
    if (head >= QUEUE_CAPACITY) {
        head = 0;
    }
    queued--;

    // Room again, so a future overflow is worth reporting afresh.
    overflow_reported = false;
    return true;
}

uint8_t count()
{
    return queued;
}

uint32_t dropped()
{
    return dropped_count;
}

void reset()
{
    head = 0;
    tail = 0;
    queued = 0;
    dropped_count = 0;
    overflow_reported = false;
}

const char *name(Kind kind)
{
    switch (kind) {
        case Kind::None:             return "None";
        case Kind::DoorOpened:       return "DoorOpened";
        case Kind::DoorClosed:       return "DoorClosed";
        case Kind::UserButtonPressed: return "UserButtonPressed";
        case Kind::CardApproved:     return "CardApproved";
        case Kind::CardDenied:       return "CardDenied";
        case Kind::InventoryUpdated: return "InventoryUpdated";
        case Kind::SquareUrlReady:   return "SquareUrlReady";
        case Kind::SquarePaid:       return "SquarePaid";
        case Kind::SquareError:      return "SquareError";
        case Kind::SquareLatePaid:   return "SquareLatePaid";
        case Kind::TouchCash:        return "TouchCash";
        case Kind::TouchOnline:      return "TouchOnline";
        case Kind::TouchBack:        return "TouchBack";
        case Kind::CoinAccepted:     return "CoinAccepted";
        case Kind::CoinRejected:     return "CoinRejected";
        case Kind::Fault:            return "Fault";
    }
    // No default label above, so adding a Kind without adding a name here is a
    // compiler warning rather than a silent "Unknown" at run time.
    return "Unknown";
}

} // namespace events
