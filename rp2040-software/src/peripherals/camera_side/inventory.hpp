// ============================================================================
// NOT BUILT. SUPERSEDED. DO NOT INCLUDE THIS FILE IN NEW CODE.
//
// Replaced by two modules that split what this one file was doing:
//
//   peripherals/catalogue/  drink types, names and THE price table
//   peripherals/basket/     shelf counts, the door-cycle diff, and the
//                           cash payment gate
//
// Removed from CMakeLists.txt so that there is exactly one definition of what
// a drink costs. Two price tables that can disagree is worse than either.
//
// Kept on disk for one idea worth preserving: simulate_dashboard_get() sketches
// the JSON shape for per-drink price overrides arriving from the dashboard.
// When the Pi link grows that message, this is the starting point — but the
// destination is catalogue, not a revived Inventory class.
// ============================================================================

// Using JSMN embedded json parser
// https://github.com/zserge/jsmn/tree/master

#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

// ---- Simulated dashboard response ----
// Stands in for a real HTTP GET until the network/cloud layer exists.
// Real version would return this same JSON shape over Wi-Fi/HTTP.
//
// DECLARED here, DEFINED in inventory.cpp. A non-inline function *defined* in a
// header gets a copy compiled into every translation unit that includes it, and
// the linker then rejects the duplicate symbols — so the previous version of
// this file could only ever be included by one source file.
std::string simulate_dashboard_get();

// Price used for a can the dashboard has no override for. Money is always
// integer cents; 200 is $2.00.
constexpr int DEFAULT_PRICE_CENTS = 200;
constexpr int DEFAULT_UNIQUE_CAN_TYPE_COUNT = 4;

enum class Can : uint8_t {
    Coke,
    Sprite,
    Fanta,
    Pasito
};

static_assert(static_cast<size_t>(Can::Pasito) < DEFAULT_UNIQUE_CAN_TYPE_COUNT,
              "Can enum has more entrees than DEFAULT_UNIQUE_CAN_TYPE_COUNT");

struct Can_state{
    int price_cents;
    int count;
};

struct Cloud_Override{
    bool has_value = false;
    int value = 0;
};

class Inventory {
public:
    Inventory() = default;
    ~Inventory() = default;

    void set_can(Can can_type, int value);
    int get_can_count(Can can_type) const;
    int get_price_cents(Can can_type) const;

    void sync_dashboard();
private:
    std::array<Can_state, DEFAULT_UNIQUE_CAN_TYPE_COUNT> m_inventory = {
        {
            {DEFAULT_PRICE_CENTS, 0}, // Coke
            {DEFAULT_PRICE_CENTS, 0}, // Sprite
            {DEFAULT_PRICE_CENTS, 0}, // Fanta
            {DEFAULT_PRICE_CENTS, 0}, // Pasito
        }
    };

    Cloud_Override fetch_price(Can can_type) const;
};