#include <stdio.h>
#include "pico/stdlib.h"
#include "peripherals/tft_display/tft_display.h"
#include "drivers/ili9341/ili9341.h"
#include "lvgl/lvgl.h"
#include "hardware/timer.h"

namespace {
    // In C++, it's much safer to use constexpr for type-safe constants rather than #define
    constexpr uint16_t DISP_HOR_RES = 240;
    constexpr uint16_t DISP_VER_RES = 320;

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

void Display::init()
{
    ili9341_init();
    lv_init();

    lv_display_t * disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    add_repeating_timer_ms(5, repeating_timer_callback, nullptr, &timer);
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