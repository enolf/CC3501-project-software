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
std::string simulate_dashboard_get() {
    return R"({
        "prices": {
            "coke": 200,
            "sprite": 250,
            "fanta": 200,
            "pasito": 0
        }
    })";
}

constexpr int DEFAULT_PRICE_CENTS = 2000;
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