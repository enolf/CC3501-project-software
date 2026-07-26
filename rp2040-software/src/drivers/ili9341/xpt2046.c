#include <stdint.h>
#include "hardware/spi.h"
#include "hardware/gpio.h"
// #include "lvgl/lvgl.h" // Adjust path to your lvgl header
#include "board.h"
#include "ili9341.h"

/** Usage

// --- LVGL v9 Input Device Registration ---

// 1. Create a new input device
lv_indev_t * indev = lv_indev_create();

// 2. Set it as a pointer (touch screen/mouse)
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

// 3. Assign your read callback function
lv_indev_set_read_cb(indev, touchpad_read);

*/

// --- SPI Communication ---
bool xpt2046_is_pressed(void) {
    // PENIRQ goes LOW when the screen is physically touched
    return gpio_get(TOUCH_IRQ_PIN) == 0;
}

uint16_t xpt2046_spi_read(uint8_t command) {
    // 1. Throttle the SPI speed down for the touch controller
    spi_set_baudrate(SPI_PORT, TOUCH_SPI_SPEED);

    uint8_t tx_buf[3] = {command, 0x00, 0x00};
    uint8_t rx_buf[3] = {0};

    // 2. Claim the bus
    gpio_put(TOUCH_CS_PIN, 0); 
    
    // 3. Execute transaction
    spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, 3); 
    
    // 4. Release the bus
    gpio_put(TOUCH_CS_PIN, 1); 

    // 5. Restore high speed for the ILI9341 so graphics don't lag
    spi_set_baudrate(SPI_PORT, DISPLAY_SPI_SPEED);

    // Combine response into a 12-bit value
    return (rx_buf[1] << 4) | (rx_buf[2] >> 4);
}