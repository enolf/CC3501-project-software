#include "inventory.hpp"


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
    for (int i = 0; i < m_inventory.size(); i++){
        Can can_type = static_cast<Can>(i);
        Cloud_Override override = fetch_price(can_type);
        m_inventory[i].price_cents = (override.has_value ? override.value : DEFAULT_PRICE_CENTS);
    }
}

Cloud_Override Inventory::fetch_price(Can can_type) const {
    

    return Cloud_Override();
}
