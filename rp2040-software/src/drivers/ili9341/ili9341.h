#ifndef ILI9341_H
#define ILI9341_H

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdbool.h>

// Hardware SPI definitions based on your exact wiring
#define ILI9341_SPI_PORT spi0
#define ILI9341_PIN_MISO 16
#define ILI9341_PIN_CS   17
#define ILI9341_PIN_SCK  18
#define ILI9341_PIN_MOSI 19

// Control Pins
#define ILI9341_PIN_DC   20
#define ILI9341_PIN_RST  21

// Backlight (P-Channel MOSFET)
#define ILI9341_PIN_BL   22

// --- Hardware Definitions (Update these to match your PCB!) ---
#define SPI_PORT          spi0
#define TOUCH_CS_PIN      24  // Your dedicated touch CS pin
#define TOUCH_IRQ_PIN     25  // Your dedicated PENIRQ pin
#define TOUCH_SPI_SPEED   2000000   // 2 MHz for XPT2046
#define DISPLAY_SPI_SPEED 30000000  // 30 MHz for ILI9341

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

#endif