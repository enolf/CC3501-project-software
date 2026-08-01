#pragma once

#include <stdint.h>
#include <stdbool.h>

// The event queue: how peripheral tasks tell the state machine that something
// happened.
//
// WHY THIS EXISTS
// ---------------
// A door task that called "start a transaction" directly would spread the
// system's behaviour across every driver, and answering "what happens when the
// door opens?" would mean reading all of them. Instead every task does one
// thing: it observes its own hardware and pushes a fact ("the door opened").
// Exactly one piece of code — run_checkout() — reads those facts and decides
// what they mean. Every state transition in the system is therefore in one
// file, and a task can never provoke one by accident.
//
// Facts, not commands: tasks push DoorOpened, never GoToSelecting.
//
// PAYLOADS ARE DELIBERATELY SMALL
// -------------------------------
// Some events refer to data far too big to sit in a queue slot — a full
// inventory, or a Square checkout URL. Those events carry no data at all; they
// are a doorbell. The state machine responds by calling the owning module for
// the actual value (pi_link::poll_inventory(), pi_link::poll_square_url()).
// That keeps every queue slot the same small size and means data is read once,
// from the module that owns it, rather than being copied through the queue.

namespace events {

/// What happened. One entry per fact the system can observe.
enum class Kind : uint8_t {
    None = 0,       ///< Never pushed; the value of a default-constructed Event.

    // --- Switches (peripherals/switches, on real hardware via DigitalSwitch) ---
    DoorOpened,
    DoorClosed,
    UserButtonPressed,  ///< The panel button. Purpose not yet assigned.

    // --- RFID (peripherals/nfc_task, real from stage 10) ---
    CardApproved,   ///< UID matched the approved list. Payload: card
    CardDenied,     ///< Card read but not on the list.  Payload: card

    // --- Pi link (peripherals/pi_link) ---
    InventoryUpdated,   ///< Doorbell: call pi_link::poll_inventory() for it.
    SquareUrlReady,     ///< Doorbell: call pi_link::poll_square_url() for it.
    SquarePaid,         ///< Square confirmed money actually arrived.
    SquareError,        ///< The Pi could not produce a payment link.
    SquareLatePaid,     ///< Money arrived AFTER the link was cancelled, so a
                        ///< refund is owed. Payload: payment

    // --- Touch (peripherals/tft_display, real from stage 6) ---
    TouchCash,
    TouchOnline,
    TouchBack,

    // --- Coin scale (peripherals/scale_task, real from stage 8) ---
    CoinAccepted,   ///< Coins identified and counted.   Payload: coin
    CoinRejected,   ///< Mass settled at an unrecognised value. Payload: coin

    // --- Faults ---
    Fault,          ///< Something is wrong enough to stop trading. Payload: fault
};

/// Largest UID the MFRC522 driver reports (7 bytes, double-size UIDs).
constexpr uint8_t EVENT_UID_MAX_LEN = 7;

/// One observed fact.
struct Event {
    Kind kind = Kind::None;

    union {
        /// CoinAccepted / CoinRejected
        struct {
            int32_t cents;          ///< Value of THIS event, not a running total
            uint8_t one_dollar;     ///< $1 coins in this event
            uint8_t two_dollar;     ///< $2 coins in this event
            float   delta_grams;    ///< Measured mass change that produced it
        } coin;

        /// CardApproved / CardDenied
        struct {
            uint8_t uid[EVENT_UID_MAX_LEN];
            uint8_t len;            ///< Valid UID bytes: 4 or 7
        } card;

        /// SquareLatePaid
        struct {
            uint32_t cents;         ///< What Square actually took
            uint32_t txn_id;        ///< Which transaction it was meant for
        } payment;

        /// Fault
        struct {
            uint16_t code;
        } fault;
    };

    // Initialising `coin` rather than `fault` is deliberate: only one union
    // member can be initialised, and coin is the largest, so this zeroes every
    // byte of the union. Initialising the smallest member would leave the rest
    // indeterminate, and a producer that filled in only some fields (a rejected
    // coin sets delta_grams but has no denomination) would leave a consumer
    // reading whatever happened to be in memory.
    Event() : kind(Kind::None), coin{} {}
    explicit Event(Kind k) : kind(k), coin{} {}
};

/// How many events can be waiting at once.
///
/// The superloop drains the queue completely on every pass, so in normal
/// operation the depth never exceeds one or two. The capacity exists to absorb
/// a burst — several coins settling as the Pi delivers an inventory — not to
/// buffer a backlog. If this ever overflows in practice, the real problem is
/// something in the loop taking too long, and the dropped count is the evidence.
constexpr uint8_t QUEUE_CAPACITY = 16;

/// Add an event to the back of the queue.
/// Returns false if the queue was full, in which case the event is DISCARDED
/// and the dropped counter is incremented.
bool push(const Event &event);

/// Convenience for the many events that carry no payload.
bool push(Kind kind);

/// Take the oldest event. Returns false when the queue is empty.
bool pop(Event &out);

/// Number of events waiting.
uint8_t count();

/// Events discarded because the queue was full since boot. Should always be 0;
/// anything else is a real defect worth investigating.
uint32_t dropped();

/// Empty the queue and clear the dropped counter.
void reset();

/// Human-readable name for a kind, for logging.
const char *name(Kind kind);

} // namespace events

// ---------------------------------------------------------------------------
// CONCURRENCY NOTE — read before adding an interrupt-driven producer
// ---------------------------------------------------------------------------
// This queue needs NO `volatile` and NO critical sections, because every
// producer and the single consumer all run in the same superloop on one core.
// There is no concurrency to protect against: push() cannot be interrupted by
// pop(), because nothing here runs in interrupt context.
//
// That is a property of the current design (CLAUDE.md section 6: poll by
// default), not a property of ring buffers. It STOPS BEING TRUE the moment any
// task raises events from an interrupt handler or from the second core, and the
// failure would be intermittent and very hard to find. If that ever happens:
//
//   * m_head, m_tail and m_count must become `volatile`, so the compiler
//     re-reads them from memory instead of trusting a cached register copy.
//   * `m_count++` and `m_count--` are read-modify-write sequences. A 32-bit
//     load or store is atomic on Cortex-M0+, but load-modify-store is NOT: an
//     interrupt landing between the read and the write silently loses an
//     update. Both need a critical section, or the count needs replacing with
//     the head/tail comparison trick that avoids a shared counter entirely.
//
// The same applies to any state a future ISR shares with the main loop.
