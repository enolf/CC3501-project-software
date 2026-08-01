#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "peripherals/basket/basket.h"
#include "peripherals/checkout/checkout.h"

// The boundary between this board and the Raspberry Pi.
//
// WHY THIS INTERFACE IS THE MOST IMPORTANT ONE IN THE PROJECT
// -----------------------------------------------------------
// The Pi is not finished, and the whole customer-facing flow depends on it: the
// camera decides what was taken, and the Square integration decides when an
// online payment has cleared. If the rest of the firmware called the Pi
// directly, nothing could be tested until the Pi worked, and every test after
// that would need a camera pointed at a fridge.
//
// So everything the Pi does sits behind these functions, and there are TWO
// implementations of them:
//
//   pi_link_sim.cpp     fabricates plausible answers, driven from the keyboard
//   pi_link_serial.cpp  the real thing, over the serial link  (stage 12)
//
// Exactly one is compiled, chosen by the PI_LINK_BACKEND option in
// CMakeLists.txt. Swapping the real Pi in later changes one word in one file,
// and nothing above this line knows the difference.
//
// THE SHAPE OF THE INTERFACE
// --------------------------
// Everything is non-blocking. Requests are fire-and-forget; answers are
// collected by polling a poll_*() function that returns true exactly once when
// something new has arrived. Nothing here ever waits for the Pi, because the
// display and the coin scale cannot afford to.
//
// The poll_*() functions pair with the doorbell events in events.h: the link
// raises InventoryUpdated, and the state machine responds by calling
// poll_inventory() for the actual data. That keeps bulky values out of the
// event queue while still letting the state machine be purely event-driven.

namespace pi_link {

/// Longest Square checkout URL accepted, including the terminating null.
/// Square's own links are around 60 characters; this leaves generous headroom
/// without putting a large buffer on the stack.
constexpr size_t URL_MAX_LEN = 128;

/// Prepare the link. Call once at startup.
void init();

/// Service the link. Call every pass of the superloop. Non-blocking.
void run();

// --- Requests to the Pi -----------------------------------------------------

/// Ask the Pi to scan the shelf and report what is on it.
///
/// Called when the door closes, because that is the only moment the camera has
/// a stable, unobstructed view. The answer arrives later as an
/// InventoryUpdated event.
void request_scan();

/// Ask the Pi to create a Square payment link for `cents`.
/// The answer arrives later as SquareUrlReady, or SquareError.
void request_square_link(uint32_t cents, uint32_t transaction_id);

// --- Answers from the Pi ----------------------------------------------------

/// Collect the most recent shelf contents.
/// Returns true exactly once per new report; false if nothing new has arrived.
bool poll_inventory(basket::Inventory &out);

/// Collect the Square checkout URL. Returns true exactly once when it arrives.
/// `out` must have room for URL_MAX_LEN bytes.
bool poll_square_url(char *out, size_t length);

/// True exactly once, when Square confirms money has actually arrived.
bool poll_square_paid();

/// True exactly once, when the Pi reports it could not produce a payment link.
bool poll_square_error();

// --- Telling the Pi what happened -------------------------------------------
// Fire-and-forget. These feed the dashboard; nothing waits on them, and a
// failure to deliver must never stop the fridge trading.

void notify_sale(const basket::Basket &basket, uint32_t cents,
                 checkout::PaymentMethod method, uint32_t transaction_id);

void notify_transaction_end(checkout::Outcome outcome, uint32_t owed_cents,
                            uint32_t paid_cents, uint32_t transaction_id);

void notify_door(bool open);

void notify_rfid(const uint8_t *uid, uint8_t uid_length, bool granted);

void notify_coin(int32_t cents, double delta_grams, bool accepted);

void notify_temperature(uint64_t rom_code, float celsius);

/// Periodic "how is this board doing" report: die temperature, how much mass is
/// sitting in the coin box, and how many faults have been raised since boot.
void notify_health(float die_celsius, double box_grams, uint32_t faults);

// --- Link health ------------------------------------------------------------

/// True while the Pi is answering. The state machine uses this to decide
/// whether it can trade at all; false should raise a fault.
bool is_healthy();

// --- Simulator hook ---------------------------------------------------------

/// Offer a debug keystroke to the link implementation.
///
/// Returns true if the key was consumed. The simulator backend uses this to let
/// the keyboard stand in for the camera and for Square; the real serial backend
/// ignores every key and returns false.
///
/// This is the one concession in an otherwise transport-agnostic interface, and
/// it earns its place: without it the simulator would need its own path to
/// stdin, and two modules reading the same input is a reliable way to lose
/// keystrokes.
bool handle_debug_key(int key);

/// One-line description of the compiled backend, for the boot banner.
const char *backend_name();

} // namespace pi_link
