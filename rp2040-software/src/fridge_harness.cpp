#include "lvgl/lvgl.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

// --- Configuration ---
#include "harness.h"

bool limit_switch_toggled() {
    static bool button_state = false;    // The official, debounced state
    static bool last_reading = false;    // The raw reading from the previous loop
    static uint32_t last_debounce_time = 0;
    
    // 1. Get the raw physical voltage on the pin (1 = 3.3V, 0 = GND)
    bool current_reading = gpio_get(LIMIT_SWITCH_BTN_PIN);
    
    // 2. If the pin flickers, reset the timer
    if (current_reading != last_reading) {
        last_debounce_time = lv_tick_get();
    }
    
    bool edge_triggered = false;
    
    // 3. If the pin has been stable for 50ms, evaluate it
    if ((lv_tick_get() - last_debounce_time) > 50) {
        
        // If the stable reading is different from our official state, update it
        if (current_reading != button_state) {
            button_state = current_reading;
            
            // If it transitioned to HIGH (3.3V), we have a valid button press
            if (button_state == true) { 
                edge_triggered = true;
            }
        }
    }
    
    // 4. Save the raw reading for the next loop's flicker check
    last_reading = current_reading;
    
    return edge_triggered;
}

// --- Timers & Callbacks ---
static void timeout_cb(lv_timer_t * timer) {
    timeout_timer = NULL; // Prevent double-deletion later
    printf("LOG: Transaction timed out after 2 minutes.\n");
    change_fridge_state(STATE_IDLE);
}

static void mock_action_cb(lv_timer_t * timer) {
    mock_action_timer = NULL; // Prevent double-deletion later
    if (current_state == STATE_PAYING_CASH || current_state == STATE_PAYING_CARD) {
        printf("LOG: Payment successful.\n");
        change_fridge_state(STATE_SUCCESS);
    } else if (current_state == STATE_SUCCESS) {
        change_fridge_state(STATE_IDLE);
    }
}

static void mock_camera_cb(lv_timer_t * timer) {
    mock_camera_timer = NULL; // Prevent double-deletion later
    
    // Simulate the Pi Camera detecting drinks being removed
    mock_cokes_taken = 0;
    mock_fantas_taken = 3; 
    current_total = (mock_cokes_taken * PRICE_COKE) + (mock_fantas_taken * PRICE_FANTA);

    if (scan_label != NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf), 
            "Your purchase:\n"
            "Coke   %d   $%.2f\n"
            "Fanta  %d   $%.2f\n\n"
            "Total:      $%.2f",
            mock_cokes_taken, (mock_cokes_taken * PRICE_COKE),
            mock_fantas_taken, (mock_fantas_taken * PRICE_FANTA),
            current_total);
        lv_label_set_text(scan_label, buf);
    }
}

// Button Events
static void payment_btn_event_cb(lv_event_t * e) {
    int payment_type = (int)(uintptr_t)lv_event_get_user_data(e);
    if (payment_type == 1) change_fridge_state(STATE_PAYING_CASH);
    else if (payment_type == 2) 
    {
        printf("CHARGE:%.2f\n", current_total);
        change_fridge_state(STATE_AWAITING_URL);
    }
    else change_fridge_state(STATE_PAYING_CARD);
}

// // --- State Machine Transition Logic ---
// void change_fridge_state(fridge_state_t new_state) {
//     current_state = new_state;
    
//     // 1. Clean up active timers
//     if (timeout_timer) { lv_timer_delete(timeout_timer); timeout_timer = NULL; }
//     if (mock_action_timer) { lv_timer_delete(mock_action_timer); mock_action_timer = NULL; }
//     if (mock_camera_timer) { lv_timer_delete(mock_camera_timer); mock_camera_timer = NULL; }
    
//     // 2. Clear the screen entirely
//     lv_obj_clean(lv_screen_active());
//     scan_label = NULL; 

//     // 3. Build the new UI based on state
//     switch (current_state) {
        
//         case STATE_IDLE: {
//             mock_cokes_taken = 0;
//             mock_fantas_taken = 0;
//             current_total = 0.0f;
            
//             lv_obj_t * lbl = lv_label_create(lv_screen_active());
//             lv_label_set_text(lbl, "Open fridge to begin");
//             lv_obj_center(lbl);
//             break;
//         }
            
//         case STATE_SCANNING: {
//             scan_label = lv_label_create(lv_screen_active());
//             lv_label_set_text(scan_label, "Scanning drinks...\n(Waiting for Pi Cam)");
//             lv_obj_center(scan_label);
            
//             // Fire fake camera data after 2 seconds
//             mock_camera_timer = lv_timer_create(mock_camera_cb, 2000, NULL);
//             lv_timer_set_repeat_count(mock_camera_timer, 1);
//             break;
//         }
            
//         case STATE_PAYMENT_SELECT: {
//             char buf[64];
//             snprintf(buf, sizeof(buf), "Your total comes to: $%.2f\nHow do you want to pay?", current_total);
            
//             lv_obj_t * lbl = lv_label_create(lv_screen_active());
//             lv_label_set_text(lbl, buf);
//             lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 40);

//             // Cash Button
//             lv_obj_t * btn_cash = lv_button_create(lv_screen_active());
//             lv_obj_set_size(btn_cash, 100, 60);
//             lv_obj_align(btn_cash, LV_ALIGN_CENTER, -60, 20);
//             lv_obj_add_event_cb(btn_cash, payment_btn_event_cb, LV_EVENT_CLICKED, (void*)1);
//             lv_obj_t * lbl_cash = lv_label_create(btn_cash);
//             lv_label_set_text(lbl_cash, "CASH");
//             lv_obj_center(lbl_cash);

//             // Card Button
//             lv_obj_t * btn_card = lv_button_create(lv_screen_active());
//             lv_obj_set_size(btn_card, 100, 60);
//             lv_obj_align(btn_card, LV_ALIGN_CENTER, 60, 20);
//             lv_obj_add_event_cb(btn_card, payment_btn_event_cb, LV_EVENT_CLICKED, (void*)2);
//             lv_obj_t * lbl_card = lv_label_create(btn_card);
//             lv_label_set_text(lbl_card, "CARD");
//             lv_obj_center(lbl_card);

//             // 2 Minute Timeout (120,000 ms)
//             timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
//             lv_timer_set_repeat_count(timeout_timer, 1);
//             break;
//         }
            
//         case STATE_PAYING_CASH: {
//             lv_obj_t * lbl = lv_label_create(lv_screen_active());
//             lv_label_set_text(lbl, "Please insert coins...\n(Simulating Load Cell)");
//             lv_obj_center(lbl);
            
//             timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
//             mock_action_timer = lv_timer_create(mock_action_cb, 5000, NULL); // 5 sec mock payment
//             lv_timer_set_repeat_count(timeout_timer, 1);
//             lv_timer_set_repeat_count(mock_action_timer, 1);
//             break;
//         }
            
//         case STATE_PAYING_CARD: {

//             // Instruction label
//             lv_obj_t * lbl = lv_label_create(lv_screen_active());
//             lv_label_set_text(lbl, "Scan to pay with square"); 
//             lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 10);

//             const char * payment_url = "https://www.berlincalling.com.au/";

//             // QR code label
//             lv_obj_t * qr = lv_qrcode_create(lv_screen_active());
//             lv_qrcode_set_size(qr, 130);
//             lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
//             lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));

//             // provide payment url to generate qr code
//             lv_qrcode_update(qr, payment_url, lv_strlen(payment_url));

//             lv_obj_align(qr, LV_ALIGN_CENTER, 0, 15);

//             lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
//             lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
            
//             timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
//             mock_action_timer = lv_timer_create(mock_action_cb, 5000, NULL); // 5 sec mock payment
//             lv_timer_set_repeat_count(timeout_timer, 1);
//             lv_timer_set_repeat_count(mock_action_timer, 1);
//             break;
//         }
            
//         case STATE_SUCCESS: {
//             lv_obj_t * lbl = lv_label_create(lv_screen_active());
//             lv_label_set_text(lbl, "Thank you for your purchase!");
//             lv_obj_center(lbl);
            
//             mock_action_timer = lv_timer_create(mock_action_cb, 5000, NULL); // 5 sec message display
//             lv_timer_set_repeat_count(mock_action_timer, 1);
//             break;
//         }
//     }
// }

// --- State Machine Poller ---
void fridge_harness_update(void) {
    if (limit_switch_toggled()) {
        printf("Button Pressed! Current state is: %d\n", current_state);
        
        if (current_state == STATE_IDLE) {
            printf("Transition: Door Opened -> Scanning\n");
            change_fridge_state(STATE_SCANNING);
        } 
        else if (current_state == STATE_SCANNING) {
            printf("Transition: Door Closed -> Payment Select\n");
            change_fridge_state(STATE_PAYMENT_SELECT);
        }
        // (Optional) Add a backdoor to cancel a transaction and return to idle
        else {
            printf("Transition: Force Reset -> Idle\n");
            change_fridge_state(STATE_IDLE);
        }
    }
}

// --- 1. Serial Command Handler ---
// This processes the strings sent from the Python script
static void handle_pi_message(const char * msg) {
    if (current_state == STATE_AWAITING_URL && strncmp(msg, "URL:", 4) == 0) {
        // Copy the URL string, skipping the "URL:" prefix
        strncpy(payment_url_buffer, msg + 4, sizeof(payment_url_buffer) - 1);
        payment_url_buffer[sizeof(payment_url_buffer) - 1] = '\0'; // Ensure null termination
        
        change_fridge_state(STATE_PAYING_CARD);
    } 
    else if (current_state == STATE_PAYING_CARD && strcmp(msg, "STATUS:PAID") == 0) {
        change_fridge_state(STATE_SUCCESS);
    }
}

// --- 2. Non-Blocking Serial Reader ---
// Runs every 50ms in the background to grab bytes from the Pi
// static void serial_reader_cb(lv_timer_t * timer) {
//     static char rx_buf[256];
//     static int rx_idx = 0;
//     int c;

//     // Read all available bytes from standard input (USB or UART0)
//     while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
//         if (c == '\n' || c == '\r') {
//             if (rx_idx > 0) {
//                 rx_buf[rx_idx] = '\0'; // Terminate string
//                 handle_pi_message(rx_buf);
//                 rx_idx = 0; // Reset for next message
//             }
//         } else if (rx_idx < 255) {
//             rx_buf[rx_idx++] = (char)c;
//         }
//     }
// }

// // --- 2. Non-Blocking Serial Reader ---
// static void serial_reader_cb(lv_timer_t * timer) {
//     static char rx_buf[256];
//     static int rx_idx = 0;
//     int c;

//     // Read all available bytes from standard input
//     while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
//         if (c == '\n' || c == '\r') {
//             if (rx_idx > 0) {
//                 rx_buf[rx_idx] = '\0'; // Terminate string
                
//                 // --- ECHO BACK TO PYTHON FOR DEBUGGING ---
//                 printf("I heard: %s\n", rx_buf);
                
//                 handle_pi_message(rx_buf);
//                 rx_idx = 0; // Reset for next message
//             }
//         } else if (rx_idx < 255) {
//             rx_buf[rx_idx++] = (char)c;
//         }
//     }
// }

static void serial_reader_cb(lv_timer_t * timer) {
    static char rx_buf[256];
    static int rx_idx = 0;
    int c;

    // Read all available bytes from standard input
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        
        // // --- DIAGNOSTIC: Echo the exact ASCII number back to Python ---
        // printf("[RAW BYTE]: %d\n", c);
        // fflush(stdout); // Force the Pico to push this log instantly
        
        if (c == '\n' || c == '\r') {
            if (rx_idx > 0) {
                rx_buf[rx_idx] = '\0'; // Terminate string
                
                // printf("I heard: %s\n", rx_buf);
                // fflush(stdout); // Force the Pico to push this log instantly
                
                handle_pi_message(rx_buf);
                rx_idx = 0; // Reset for next message
            }
        } else if (rx_idx < 255) {
            rx_buf[rx_idx++] = (char)c;
        }
    }
}

// --- 4. State Machine Updates ---
void change_fridge_state(fridge_state_t new_state) {
    current_state = new_state;
    
    // Clean up active timers safely
    if (timeout_timer) { lv_timer_delete(timeout_timer); timeout_timer = NULL; }
    if (mock_action_timer) { lv_timer_delete(mock_action_timer); mock_action_timer = NULL; }
    if (mock_camera_timer) { lv_timer_delete(mock_camera_timer); mock_camera_timer = NULL; }
    
    // Safety guard
    lv_obj_t * active_screen = lv_screen_active();
    if (active_screen == NULL) return; 

    lv_obj_clean(active_screen);
    scan_label = NULL; 

    // Ensure the serial reader is always running
    if (serial_reader_timer == NULL) {
        serial_reader_timer = lv_timer_create(serial_reader_cb, 50, NULL);
    }

    switch (current_state) {

        case STATE_IDLE: {
            mock_cokes_taken = 0;
            mock_fantas_taken = 0;
            current_total = 0.0f;
            
            lv_obj_t * lbl = lv_label_create(lv_screen_active());
            lv_label_set_text(lbl, "Open fridge to begin");
            lv_obj_center(lbl);
            break;
        }
            
        case STATE_SCANNING: {
            scan_label = lv_label_create(lv_screen_active());
            lv_label_set_text(scan_label, "Scanning drinks...\n(Waiting for Pi Cam)");
            lv_obj_center(scan_label);
            
            // Fire fake camera data after 2 seconds
            mock_camera_timer = lv_timer_create(mock_camera_cb, 2000, NULL);
            lv_timer_set_repeat_count(mock_camera_timer, 1);
            break;
        }

        case STATE_PAYMENT_SELECT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "Your total comes to: $%.2f\nHow do you want to pay?", current_total);
            
            lv_obj_t * lbl = lv_label_create(lv_screen_active());
            lv_label_set_text(lbl, buf);
            lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 40);

            // Cash Button
            lv_obj_t * btn_cash = lv_button_create(lv_screen_active());
            lv_obj_set_size(btn_cash, 100, 60);
            lv_obj_align(btn_cash, LV_ALIGN_CENTER, -60, 20);
            lv_obj_add_event_cb(btn_cash, payment_btn_event_cb, LV_EVENT_CLICKED, (void*)1);
            lv_obj_t * lbl_cash = lv_label_create(btn_cash);
            lv_label_set_text(lbl_cash, "CASH");
            lv_obj_center(lbl_cash);

            // Card Button
            lv_obj_t * btn_card = lv_button_create(lv_screen_active());
            lv_obj_set_size(btn_card, 100, 60);
            lv_obj_align(btn_card, LV_ALIGN_CENTER, 60, 20);
            lv_obj_add_event_cb(btn_card, payment_btn_event_cb, LV_EVENT_CLICKED, (void*)2);
            lv_obj_t * lbl_card = lv_label_create(btn_card);
            lv_label_set_text(lbl_card, "CARD");
            lv_obj_center(lbl_card);

            // 2 Minute Timeout (120,000 ms)
            timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
            lv_timer_set_repeat_count(timeout_timer, 1);
            break;
        }

        case STATE_PAYING_CASH: {
            lv_obj_t * lbl = lv_label_create(lv_screen_active());
            lv_label_set_text(lbl, "Please insert coins...\n(Simulating Load Cell)");
            lv_obj_center(lbl);
            
            timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
            mock_action_timer = lv_timer_create(mock_action_cb, 5000, NULL); // 5 sec mock payment
            lv_timer_set_repeat_count(timeout_timer, 1);
            lv_timer_set_repeat_count(mock_action_timer, 1);
            break;
        }

        case STATE_AWAITING_URL: {
            lv_obj_t * lbl = lv_label_create(active_screen);
            lv_label_set_text(lbl, "Contacting Square...\nGenerating Payment Link");
            lv_obj_center(lbl);
            
            timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
            lv_timer_set_repeat_count(timeout_timer, 1);
            break;
        }
            
        case STATE_PAYING_CARD: {
            lv_obj_t * lbl = lv_label_create(active_screen);
            lv_label_set_text(lbl, "Scan to pay with Square:");
            lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 10);
            
            lv_obj_t * qr = lv_qrcode_create(active_screen);
            lv_qrcode_set_size(qr, 130);
            lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
            lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
            
            // Use the dynamically received URL
            lv_qrcode_update(qr, payment_url_buffer, strlen(payment_url_buffer));
            
            lv_obj_align(qr, LV_ALIGN_CENTER, 0, 15);
            lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(qr, 4, 0);

            // 2 Minute timeout for the user to scan and pay
            timeout_timer = lv_timer_create(timeout_cb, 120000, NULL);
            lv_timer_set_repeat_count(timeout_timer, 1);
            break;
        }

        case STATE_SUCCESS: {
            lv_obj_t * lbl = lv_label_create(lv_screen_active());
            lv_label_set_text(lbl, "Thank you for your purchase!");
            lv_obj_center(lbl);
            
            mock_action_timer = lv_timer_create(mock_action_cb, 5000, NULL); // 5 sec message display
            lv_timer_set_repeat_count(mock_action_timer, 1);
            break;
        }
    }
}