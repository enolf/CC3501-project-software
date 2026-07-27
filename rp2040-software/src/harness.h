#pragma once

#include "lvgl/lvgl.h"
#include "hardware/gpio.h"
#include <stdio.h>

#define LIMIT_SWITCH_BTN_PIN 15 // Your physical user button on the PCB

// Drink Prices
#define PRICE_COKE  2.50f
#define PRICE_FANTA 2.50f

// --- System States ---
typedef enum {
    STATE_IDLE,
    STATE_SCANNING,
    STATE_PAYMENT_SELECT,
    STATE_PAYING_CASH,
    STATE_PAYING_CARD,
    STATE_SUCCESS
} fridge_state_t;

// --- Global Variables ---
static fridge_state_t current_state = STATE_IDLE;

static int mock_cokes_taken = 0;
static int mock_fantas_taken = 0;
static float current_total = 0.0f;


// Timers
static lv_timer_t * timeout_timer = NULL;
static lv_timer_t * mock_action_timer = NULL;
static lv_timer_t * mock_camera_timer = NULL;

// UI Pointers (for live updates during scanning)
static lv_obj_t * scan_label = NULL;

// Forward Declarations
void change_fridge_state(fridge_state_t new_state);
bool limit_switch_toggled();
void fridge_harness_update();