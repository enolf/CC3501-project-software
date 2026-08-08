#include "ili9341.h"
#include "board.h"

// Helper macros for Chip Select and Data/Command toggling
static inline void cs_select()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(DISPLAY_CS_PIN, 0);
    asm volatile("nop \n nop \n nop");
}

static inline void cs_deselect()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(DISPLAY_CS_PIN, 1);
    asm volatile("nop \n nop \n nop");
}

static inline void dc_command()
{
    gpio_put(DISPLAY_DC_PIN, 0);
}

static inline void dc_data()
{
    gpio_put(DISPLAY_DC_PIN, 1);
}

// WHY THE SPI RETURN VALUES BELOW ARE NOT CHECKED, when every I2C call in
// mfrc522.cpp is. The rule (CLAUDE.md section 7) exists because a bus
// transaction can fail and report it; I2C genuinely can, since an absent or
// wedged device NACKs its address and i2c_write_blocking() returns
// PICO_ERROR_GENERIC. SPI has no acknowledgement in the protocol at all: the
// controller clocks bits out regardless of whether anything is listening, and
// spi_write_blocking() only returns once it has transferred every byte it was
// given. Its return value is therefore always equal to the length passed in,
// and a check against it could never be false.
//
// So the failure a check would be reaching for is not detectable here. It is
// caught instead by reading the display back where the chip supports it, and
// by the fact that a dead panel is immediately visible. Writing `if (n != 1)`
// around each of these would look like diligence while testing nothing.
static void ili9341_write_cmd(uint8_t cmd)
{
    dc_command();
    cs_select();
    spi_write_blocking(DISPLAY_SPI_INSTANCE, &cmd, 1);
    cs_deselect();
}

static void ili9341_write_data(uint8_t data)
{
    dc_data();
    cs_select();
    spi_write_blocking(DISPLAY_SPI_INSTANCE, &data, 1);
    cs_deselect();
}

void ili9341_init(void)
{
    // 1. Initialise SPI at the display's rate (fast enough for smooth UI).
    // The touch driver drops the bus to TOUCH_SPI_BAUDRATE around each of its
    // transactions and puts it back afterwards.
    spi_init(DISPLAY_SPI_INSTANCE, DISPLAY_SPI_BAUDRATE);
    gpio_set_function(DISPLAY_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(DISPLAY_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(DISPLAY_MISO_PIN, GPIO_FUNC_SPI);

    // 2. Initialize Control Pins
    gpio_init(DISPLAY_CS_PIN);
    gpio_set_dir(DISPLAY_CS_PIN, GPIO_OUT);
    gpio_put(DISPLAY_CS_PIN, 1);

    gpio_init(DISPLAY_DC_PIN);
    gpio_set_dir(DISPLAY_DC_PIN, GPIO_OUT);
    gpio_put(DISPLAY_DC_PIN, 1);

    gpio_init(DISPLAY_RST_PIN);
    gpio_set_dir(DISPLAY_RST_PIN, GPIO_OUT);
    gpio_put(DISPLAY_RST_PIN, 1);

    // 3. Backlight Control Setup
    gpio_init(DISPLAY_BL_PIN);
    gpio_set_dir(DISPLAY_BL_PIN, GPIO_OUT);  

    // Backlight on. Which logic level means "on" is a board wiring fact, so it
    // comes from DISPLAY_BL_ACTIVE_LOW in board.h rather than from editing this
    // driver when moving between the breadboard prototype (active-high NPN) and
    // the custom PCB (active-low P-channel MOSFET).
    gpio_put(DISPLAY_BL_PIN, DISPLAY_BL_ACTIVE_LOW ? 0 : 1);

    // 4. Hardware Reset
    gpio_put(DISPLAY_RST_PIN, 0);
    sleep_ms(50);
    gpio_put(DISPLAY_RST_PIN, 1);
    sleep_ms(150);

    // 5. Send ILI9341 Initialization Sequence
    ili9341_write_cmd(0x01); // Software Reset
    sleep_ms(150);

    ili9341_write_cmd(0x11); // Sleep Out
    sleep_ms(255);

    ili9341_write_cmd(0x3A); // Pixel Format
    ili9341_write_data(0x55); // 16-bit colors (RGB565)

    // ili9341_write_cmd(0x36); // Memory Access Control
    // ili9341_write_data(0x48); // BGR order (fixes inverted colors on red boards)

    // ORIENTATION
    // Below are the supported LANDSCAPE orientations. Configure in "board.h"
    // so changes are global and affect touch and the graphics softare (LVGL)

    // 0xA8 VCC Pin on the top
    // 0x68 VCC Pin on the bottom

    ili9341_write_cmd(0x36); // Memory Access Control
    ili9341_write_data(DISP_H_V_CONF);  

    ili9341_write_cmd(0x29); // Display ON
    sleep_ms(150);
}

void ili9341_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    ili9341_write_cmd(0x2A); // Column Address Set
    ili9341_write_data(x1 >> 8);
    ili9341_write_data(x1 & 0xFF);

    ili9341_write_data(x2 >> 8);
    ili9341_write_data(x2 & 0xFF);

    ili9341_write_cmd(0x2B); // Page Address Set
    ili9341_write_data(y1 >> 8);
    ili9341_write_data(y1 & 0xFF);

    ili9341_write_data(y2 >> 8);
    ili9341_write_data(y2 & 0xFF);

    ili9341_write_cmd(0x2C); // Memory Write
}

void ili9341_write_pixels(const uint16_t *data, size_t len)
{
    dc_data();
    cs_select();
    // Use blocking SPI for now. This can be upgraded to DMA later for max performance!
    spi_write_blocking(DISPLAY_SPI_INSTANCE, (const uint8_t *)data, len * 2);
    cs_deselect();
}