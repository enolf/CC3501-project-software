#pragma once

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdbool.h>

// Wiring (pins, SPI instance, bus rates, backlight polarity) lives in board.h.
// Only chip facts belong in this header.
#include "board.h"

// XPT2046 Control Bytes
#define CMD_READ_X 0xD0
#define CMD_READ_Y 0x90

// --- C++ COMPATIBILITY WRAPPER ---
// This tells the C++ compiler to use standard C linkage for these functions
// so it doesn't mangle their names during compilation.
#ifdef __cplusplus
extern "C" {
#endif

// Function prototypes
void ili9341_init(void);
void ili9341_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void ili9341_write_pixels(const uint16_t *data, size_t len);
void touch_init(void);
bool xpt2046_is_pressed(void);
uint16_t xpt2046_spi_read(uint8_t command);

#ifdef __cplusplus
}
#endif