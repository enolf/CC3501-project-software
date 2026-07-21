#include <stdio.h>
// #include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "WS2812.pio.h" // This header file gets produced during compilation from the WS2812.pio file
#include "drivers/logging/logging.h"

#include "peripherals/tft_display.h"

int main()
{
    stdio_init_all();

    Display::init();
    Display::write_text("CAN YOU SEE ME?");

    for (;;) {
        Display::run();
        sleep_ms(5);
    }

    return 0;
}
