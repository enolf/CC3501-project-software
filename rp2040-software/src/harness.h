#pragma once

#include "lvgl/lvgl.h"
#include "hardware/gpio.h"
#include <stdio.h>

#include <stdio.h>
#include <string.h>

#define USER_BTN_PIN 15 // GPIO Pin of User Tactile Switch
#define LIMIT_SWITCH_BTN_PIN USER_BTN_PIN // LIM SW is GPIO Pin 6

// --- Drink Definitions ---
#define COST_COKE    2.50f
#define COST_FANTA   2.50f
#define COST_PASSITO 2.50f
#define COST_SOLO    2.50f

typedef struct {
    const char* name;
    float price;
    int initial_count;
    int current_count;
    int delta;
} DrinkItem;

// Alphabetical order: Coke, Fanta, Passito, Solo
static DrinkItem cart[4] = {
    {"Coke",    COST_COKE,    -1, 0, 0},
    {"Fanta",   COST_FANTA,   -1, 0, 0},
    {"Passito", COST_PASSITO, -1, 0, 0},
    {"Solo",    COST_SOLO,    -1, 0, 0}
};

// --- System States ---
typedef enum {
    STATE_IDLE,
    STATE_SCANNING,
    STATE_PAYMENT_SELECT,
    STATE_PAYING_CASH,
    STATE_PAYING_CARD,
    STATE_AWAITING_URL,
    STATE_SUCCESS
} fridge_state_t;

// --- Global Variables ---
static fridge_state_t current_state = STATE_IDLE;

static int mock_cokes_taken = 0;
static int mock_fantas_taken = 0;
static float current_total = 0.0f;

// --- New Global Variables ---
static char payment_url_buffer[256];
static lv_timer_t * serial_reader_timer = NULL;
static lv_timer_t * square_poll_timer = NULL;


// Timers
static lv_timer_t * timeout_timer = NULL;
static lv_timer_t * mock_action_timer = NULL;

// UI Pointers (for live updates during scanning)
static lv_obj_t * scan_label = NULL;

// Forward Declarations
void change_fridge_state(fridge_state_t new_state);
bool limit_switch_toggled();
void fridge_harness_update();