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
///
/// THE ORDER MATCHES THE VISION PROGRAM'S `colors_vec`. picapture classifies
/// pixels against a list of brand colours and reports them positionally, so a
/// drink inserted in the middle here without the same edit there silently
/// relabels every count.
enum class Can : uint8_t {
    Coke        = 0,
    Fanta       = 1,
    MountainDew = 2,
    Solo        = 3,
};

/// How many drink types exist. Used to size every per-drink array.
constexpr uint8_t CAN_COUNT = 4;

/// One row of the catalogue.
struct Entry {
    const char *name;       ///< Shown on the TFT and on the customer's receipt
    const char *wire_key;   ///< How this drink is named in a serial payload
    uint32_t    price_cents;
};

/// ===== THE PRICE TABLE =====
/// Order must match the Can enum above.
///
/// WHY `wire_key` IS NOT JUST `name` LOWERCASED
/// --------------------------------------------
/// It used to be: the INV handler lowercased name() to build the key it looked
/// for. That worked only for as long as every drink happened to be one word.
/// "Mountain Dew" broke it, and not visibly — a payload is space-separated
/// `key=value` pairs, so `mountain dew=5` splits into a key nobody asked for
/// and a stray token, and `items=Mountain Dew:1 owed=200` truncates to
/// `items=Mountain` at the first space. Both ends would have gone on running
/// and quietly agreed on the wrong numbers.
///
/// So the wire name is now its own column: short, lower case, no spaces, and
/// free to differ from what the customer reads on the screen. Keep it that way
/// — a drink named "Solo & Lime" must not be able to reach the link.
inline constexpr Entry TABLE[CAN_COUNT] = {
    /* Coke        */ { "Coke",         "coke",   200 },
    /* Fanta       */ { "Fanta",        "fanta",  200 },
    /* MountainDew */ { "Mountain Dew", "mtndew", 200 },
    /* Solo        */ { "Solo",         "solo",   200 },
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

/// How this drink is named in a serial payload. ALWAYS use this, never name(),
/// when building or reading a frame — see the note on the table above.
///
/// Returns "unknown" rather than reading past the end of the table. That is a
/// key no sender will ever emit, so an out-of-range value fails to match
/// instead of matching the wrong drink.
constexpr const char *wire_key(Can can)
{
    return is_valid(can) ? TABLE[static_cast<uint8_t>(can)].wire_key : "unknown";
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
