#include <stdio.h>
#include "load_cell.h"
#include "drivers/hx711/common.h"

void Load_cell::init()
{
    hx711_t hx = {0};
    hx711_config_t hxcfg = {0};
    hx711_get_default_config(&hxcfg);

    // Set GPIO pins DATA and CLOCK 
    hxcfg.clock_pin = HX711_CLK_GPIO_PIN;
    hxcfg.data_pin = HX711_DATA_GPIO_PIN;

    // Initialize the hardware using the config struct
    hx711_init(&hx, &hxcfg);
}

void Load_cell::start()
{
    //Power up the hx711 and set gain on chip
    hx711_power_up(&hx, hx711_gain_128);
    
    hx711_wait_settle(hx711_rate_80);
}

void Load_cell::read_and_print()
{
    // wait (block) until a value is obtained
    printf("blocking value: %li\n", hx711_get_value(&hx));

    // or use a timeout
    // int32_t val;
    // const uint timeout = 250000; //microseconds
    // if(hx711_get_value_timeout(&hx, timeout, &val)) {
    //     // value was obtained within the timeout period
    //     printf("timeout value: %li\n", val);
    // }
    // else {
    //     printf("value was not obtained within the timeout period\n");
    // }

    // // or see if there's a value, but don't block if there isn't one ready
    // if(hx711_get_value_noblock(&hx, &val)) {
    //     printf("noblock value: %li\n", val);
    // }
    // else {
    //     printf("value was not present\n");
    // }
}

void Load_cell::stop()
{
    // Stop communication with HX711
    hx711_close(&hx);
}