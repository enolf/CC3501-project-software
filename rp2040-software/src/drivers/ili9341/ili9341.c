#include "ili9341.h"

// Helper macros for Chip Select and Data/Command toggling
static inline void cs_select()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(ILI9341_PIN_CS, 0);
    asm volatile("nop \n nop \n nop");
}

static inline void cs_deselect()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(ILI9341_PIN_CS, 1);
    asm volatile("nop \n nop \n nop");
}

static inline void dc_command()
{
    gpio_put(ILI9341_PIN_DC, 0);
}

static inline void dc_data()
{
    gpio_put(ILI9341_PIN_DC, 1);
}

static void ili9341_write_cmd(uint8_t cmd)
{
    dc_command();
    cs_select();
    spi_write_blocking(ILI9341_SPI_PORT, &cmd, 1);
    cs_deselect();
}

static void ili9341_write_data(uint8_t data)
{
    dc_data();
    cs_select();
    spi_write_blocking(ILI9341_SPI_PORT, &data, 1);
    cs_deselect();
}

void ili9341_init(void)
{
    // 1. Initialize SPI at 30 MHz (fast enough for smooth UI)
    spi_init(ILI9341_SPI_PORT, 30 * 1000 * 1000);
    gpio_set_function(ILI9341_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(ILI9341_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(ILI9341_PIN_MISO, GPIO_FUNC_SPI);

    // 2. Initialize Control Pins
    gpio_init(ILI9341_PIN_CS);
    gpio_set_dir(ILI9341_PIN_CS, GPIO_OUT);
    gpio_put(ILI9341_PIN_CS, 1);

    gpio_init(ILI9341_PIN_DC);
    gpio_set_dir(ILI9341_PIN_DC, GPIO_OUT);
    gpio_put(ILI9341_PIN_DC, 1);

    gpio_init(ILI9341_PIN_RST);
    gpio_set_dir(ILI9341_PIN_RST, GPIO_OUT);
    gpio_put(ILI9341_PIN_RST, 1);

    // 3. Backlight Control Setup
    gpio_init(ILI9341_PIN_BL);
    gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);  

    // ---------------------------------------------------------
    // --- PROTOTYPE MODE (PICO BREADBOARD) ---
    // If you hardwired the LED pin to 3V3 on the breadboard, this 
    // does nothing. If you used a standard active-high NPN transistor
    // to test switching, HIGH (1) turns the screen ON.
    gpio_put(ILI9341_PIN_BL, 1); 

    // --- CUSTOM PCB MODE (P-CHANNEL MOSFET) ---
    // UNCOMMENT the line below and comment out the prototype line 
    // above when flashing to your custom PCB! 
    // A P-Channel MOSFET requires a LOW (0) signal to turn ON.
    // gpio_put(ILI9341_PIN_BL, 0); 
    // ---------------------------------------------------------

    // 4. Hardware Reset
    gpio_put(ILI9341_PIN_RST, 0);
    sleep_ms(50);
    gpio_put(ILI9341_PIN_RST, 1);
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

    // --- THE MIRROR FIX ---
    // 0x36 controls orientation.
    // 0x48 = Mirrored X-axis + BGR
    // 0x08 = Normal X-axis + BGR
    // 0xC8 = Mirrored X AND Mirrored Y + BGR    
    ili9341_write_cmd(0x36); // Memory Access Control
    ili9341_write_data(0xC8); // Set to 180 degrees

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
    spi_write_blocking(ILI9341_SPI_PORT, (const uint8_t *)data, len * 2);
    cs_deselect();
}