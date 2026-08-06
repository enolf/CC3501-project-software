#include "inventory.hpp"

std::string simulate_dashboard_get() {
    return R"({
        "prices": {
            "coke": 200,
            "fanta": 250,
            "mtndew": 200,
            "solo": 0
        }
    })";
}

void Inventory::set_can(Can can_type, int value) {
    m_inventory[static_cast<size_t>(can_type)].count = value;
}

int Inventory::get_can_count(Can can_type) const {
    return m_inventory[static_cast<size_t>(can_type)].count;
}

int Inventory::get_price_cents(Can can_type) const {
    return m_inventory[static_cast<size_t>(can_type)].price_cents;
}

void Inventory::sync_dashboard() {
    for (size_t i = 0; i < m_inventory.size(); i++){
        Can can_type = static_cast<Can>(i);
        Cloud_Override override = fetch_price(can_type);

        // Only an override with a value changes anything. The previous version
        // fell back to DEFAULT_PRICE_CENTS whenever there was no override,
        // which meant every call quietly reset every price that had been set —
        // and since fetch_price() is still a stub returning "no value", a sync
        // wiped the whole table.
        if (override.has_value) {
            m_inventory[i].price_cents = override.value;
        }
    }
}

Cloud_Override Inventory::fetch_price(Can /*can_type*/) const {
    // STUB. Returns "no override", so sync_dashboard() leaves every price
    // alone. Wiring this up means parsing simulate_dashboard_get()'s JSON —
    // and ultimately the real value arriving from the Pi over the serial link.
    // The parameter is unnamed until then, so the compiler does not warn about
    // an argument this stub cannot yet use.
    return Cloud_Override();
}
