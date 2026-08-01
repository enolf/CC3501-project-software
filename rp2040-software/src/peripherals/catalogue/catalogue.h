#pragma once

#include <stdint.h>

// What the fridge sells, and what it costs.
//
// THE PRICE TABLE BELOW IS THE ONLY PLACE A PRICE APPEARS. Changing what a
// drink costs is a one-line edit here and nothing else in the system needs to
// know. Nowhere else may contain a literal price — not the display, not the
// checkout, not the Square request. Everything asks price_cents().
//
// MONEY IS ALWAYS INTEGER CENTS. Never float, never double, never "dollars".
// Binary floating point cannot represent 0.10 exactly, so repeated addition of
// prices drifts, and a total that is a hair under the amount owed would leave a
// customer unable to complete a payment they had actually made. Integers cannot
// do that.
//
// This header is deliberately free of any hardware dependency, so it compiles
// unchanged into the off-hardware unit tests.

namespace catalogue {

/// The drink types the camera can distinguish. The Pi reports counts in this
/// same order, so the numeric values matter: they index the table below and the
/// inventory arrays in basket.h.
enum class Can : uint8_t {
    Coke   = 0,
    Sprite = 1,
    Fanta  = 2,
    Pasito = 3,
};

/// How many drink types exist. Used to size every per-drink array.
constexpr uint8_t CAN_COUNT = 4;

/// One row of the catalogue.
struct Entry {
    const char *name;       ///< Shown on the TFT and in the sale record
    uint32_t    price_cents;
};

/// ===== THE PRICE TABLE =====
/// Order must match the Can enum above.
inline constexpr Entry TABLE[CAN_COUNT] = {
    /* Coke   */ { "Coke",   200 },
    /* Sprite */ { "Sprite", 200 },
    /* Fanta  */ { "Fanta",  200 },
    /* Pasito */ { "Pasito", 200 },
};

/// True if `value` is a drink type this build knows about. Worth checking
/// before indexing anything, because a count arriving over the serial link is
/// data from another machine, not a value this program created.
constexpr bool is_valid(Can can)
{
    return static_cast<uint8_t>(can) < CAN_COUNT;
}

/// Display name, or "Unknown" for an out-of-range value rather than reading
/// past the end of the table.
constexpr const char *name(Can can)
{
    return is_valid(can) ? TABLE[static_cast<uint8_t>(can)].name : "Unknown";
}

/// Price in whole cents. Returns 0 for an out-of-range value; a caller that
/// might be handed one should check is_valid() first, because a free drink is
/// not a sensible fallback.
constexpr uint32_t price_cents(Can can)
{
    return is_valid(can) ? TABLE[static_cast<uint8_t>(can)].price_cents : 0;
}

/// Convert a raw index (from a serial message, or a loop counter) to a Can.
constexpr Can from_index(uint8_t index)
{
    return static_cast<Can>(index);
}

} // namespace catalogue
