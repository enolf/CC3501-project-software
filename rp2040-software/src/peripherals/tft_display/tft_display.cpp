#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "peripherals/tft_display/tft_display.h"
#include "lvgl/lvgl.h"
#include "drivers/ili9341/ili9341.h"
#include "hardware/timer.h"
#include "board.h"

namespace {
    // LVGL render buffer
    static lv_color_t buf[DISP_HOR_RES * DISP_VER_RES / 10];

    struct repeating_timer timer;

    // The critical LVGL flush callback. 
    // Because this is passed to a pure C library (LVGL), it's good practice
    // to ensure it doesn't throw exceptions and maintains static linkage.
    static void my_disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
        auto * color_p = reinterpret_cast<uint16_t *>(px_map);
        
        ili9341_set_window(area->x1, area->y1, area->x2, area->y2);
        
        size_t len = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
        ili9341_write_pixels(color_p, len);
        
        lv_display_flush_ready(disp);
    }

    // Hardware timer callback
    bool repeating_timer_callback(struct repeating_timer *t) {
        lv_tick_inc(5); 
        return true;
    }
}

// --- Math Helper ---
int16_t map_coordinate(int16_t val, int16_t in_min, int16_t in_max, int16_t out_min, int16_t out_max) {
    int32_t result = (int32_t)(val - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    
    // Clamp the output so we don't return coordinates off-screen
    if (out_min < out_max) {
        if (result < out_min) return out_min;
        if (result > out_max) return out_max;
    } else {
        if (result > out_min) return out_min;
        if (result < out_max) return out_max;
    }
    return (int16_t)result;
}

// Callback for touch screen pointing to LVGL 
void touchpad_read(lv_indev_t * indev_core, lv_indev_data_t * data) {
    if (xpt2046_is_pressed()) {
        data->state = LV_INDEV_STATE_PRESSED; // Updated for v9
        
        // 1. Grab raw ADC values
        uint16_t raw_x = xpt2046_spi_read(CMD_READ_X);
        uint16_t raw_y = xpt2046_spi_read(CMD_READ_Y);
        
        int16_t calc_x = raw_x;
        int16_t calc_y = raw_y;

        // 2 & 3. Handle Rotation and Mapping
        if (TOUCH_SWAP_XY) {
            calc_x = raw_y;
            calc_y = raw_x;
            
            // Because axes are swapped, map X using Y's physical boundaries
            if (TOUCH_INVERT_X) data->point.x = map_coordinate(calc_x, TOUCH_Y_MIN, TOUCH_Y_MAX, DISP_HOR_RES, 0);
            else                data->point.x = map_coordinate(calc_x, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, DISP_HOR_RES);

            // And map Y using X's physical boundaries
            if (TOUCH_INVERT_Y) data->point.y = map_coordinate(calc_y, TOUCH_X_MIN, TOUCH_X_MAX, DISP_VER_RES, 0);
            else                data->point.y = map_coordinate(calc_y, TOUCH_X_MIN, TOUCH_X_MAX, 0, DISP_VER_RES);
            
        } else {
            // Normal axis mapping
            if (TOUCH_INVERT_X) data->point.x = map_coordinate(calc_x, TOUCH_X_MIN, TOUCH_X_MAX, DISP_HOR_RES, 0);
            else                data->point.x = map_coordinate(calc_x, TOUCH_X_MIN, TOUCH_X_MAX, 0, DISP_HOR_RES);

            if (TOUCH_INVERT_Y) data->point.y = map_coordinate(calc_y, TOUCH_Y_MIN, TOUCH_Y_MAX, DISP_VER_RES, 0);
            else                data->point.y = map_coordinate(calc_y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, DISP_VER_RES);
        }
        

        // DIAGNOSTIC PRINT -- Uncomment to calibrate
        if(CALIBRATE_TOUCH_MODE)
        {
            printf("TOUCH PRESSED | Raw: X=%04u, Y=%04u | Mapped: X=%03d, Y=%03d\n", 
                   raw_x, raw_y, data->point.x, data->point.y);
        }

    } else {
        data->state = LV_INDEV_STATE_RELEASED; // Updated for v9
    }
}

uint32_t pico_tick_get_cb(void) {
    // time_us_64() is a standard Pico SDK function. 
    // Dividing by 1000 gives us the milliseconds LVGL expects.
    return (uint32_t)(time_us_64() / 1000); 
}

// --- Touch Initialization ---
void touch_init(void) {
    // Initialize CS pin
    gpio_init(TOUCH_CS_PIN);
    gpio_set_dir(TOUCH_CS_PIN, GPIO_OUT);
    gpio_put(TOUCH_CS_PIN, 1); // Deselect by default (HIGH)

    // --- THE SPI FLUSH (Valid Command Fix) ---
    spi_set_baudrate(SPI_PORT, TOUCH_SPI_SPEED); 
    
    gpio_put(TOUCH_CS_PIN, 0); 
    
    // Send 0xD0 (CMD_READ_X) to force a conversion and trigger power-down mode
    uint8_t dummy_tx[3] = {CMD_READ_X, 0x00, 0x00};
    uint8_t dummy_rx[3] = {0};
    
    // Read and discard to clear the Pico's internal RX FIFO
    spi_write_read_blocking(SPI_PORT, dummy_tx, dummy_rx, 3);
    
    gpio_put(TOUCH_CS_PIN, 1); 
    
    spi_set_baudrate(SPI_PORT, DISPLAY_SPI_SPEED);
    // ---------------------------------------

    // Initialize IRQ pin
    gpio_init(TOUCH_IRQ_PIN);
    gpio_set_dir(TOUCH_IRQ_PIN, GPIO_IN);
    gpio_pull_up(TOUCH_IRQ_PIN); // Keep HIGH until pressed

    // 1. Create a new input device
    lv_indev_t * indev = lv_indev_create();

    // 2. Set it as a pointer (touch screen/mouse)
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    // 3. Assign your read callback function
    lv_indev_set_read_cb(indev, touchpad_read);


}

void Display::init()
{
    ili9341_init();
    lv_init();

    lv_display_t * disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    add_repeating_timer_ms(5, repeating_timer_callback, nullptr, &timer);
    
     // tell LVGL how to get the current time has passed so it knows how fast to slide switches
    lv_tick_set_cb(pico_tick_get_cb);

    touch_init();
}

void Display::write_text(const char *string)
{
    // --- CREATE THE "HELLO WORLD" UI ---
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), LV_PART_MAIN); 

    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, string);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN); 
    lv_obj_center(label);
}

void Display::run()
{
    lv_timer_handler(); 
}

// --- 1. The Event Callback ---
// This function runs automatically whenever a switch is tapped
static void switch_event_cb(lv_event_t * e) {
    // Get the object that triggered the event (the switch)
    lv_obj_t * sw = lv_event_get_target_obj(e);
    
    // Get the custom user data we passed in (so we know WHICH switch was tapped)
    int switch_id = (int)(uintptr_t)lv_event_get_user_data(e);

    // Check the new state of the switch
    if(lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        printf("Switch %d was turned ON!\n", switch_id);
        // Put your RP2040 logic here (e.g., turn on a GPIO, send a UART message)
    } 
    else {
        printf("Switch %d was turned OFF!\n", switch_id);
        // Put your RP2040 logic here
    }
}

// --- 2. Creating the UI ---
// Call this function inside your main loop after LVGL is initialized
void Display::create_dual_switches(void) {
    
    // --- Switch 1 ---
    lv_obj_t * sw1 = lv_switch_create(lv_screen_active());
    
    // Align it to the center of the screen, but shift it UP by 40 pixels
    lv_obj_align(sw1, LV_ALIGN_CENTER, 0, -40); 
    
    // Attach the event callback. We pass '(void*)1' as user data to identify it as Switch 1.
    lv_obj_add_event_cb(sw1, switch_event_cb, LV_EVENT_VALUE_CHANGED, (void*)1);


    // --- Switch 2 ---
    lv_obj_t * sw2 = lv_switch_create(lv_screen_active());
    
    // Align it to the center, but shift it DOWN by 40 pixels
    lv_obj_align(sw2, LV_ALIGN_CENTER, 0, 40); 
    
    // Attach the exact same callback, but pass '(void*)2' so we know it's Switch 2
    lv_obj_add_event_cb(sw2, switch_event_cb, LV_EVENT_VALUE_CHANGED, (void*)2);
    
    // (Optional) Manually set Switch 2 to the ON position by default during startup
    lv_obj_add_state(sw2, LV_STATE_CHECKED);
}