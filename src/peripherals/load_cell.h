#pragma once

#include "pico-scale/extern/hx711-pico-c/include/common.h"

#define HX711_CLK_GPIO_PIN 10
#define HX711_DATA_GPIO_PIN 11

class Load_cell
{
    public:
        hx711_t hx;
        hx711_config_t hxcfg;
        void init();
        void start();
        void read_and_print();
        void stop();
        int calibrate();
};