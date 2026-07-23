// MIT License

// Copyright (c) 2023 Daniel Robertson

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdio.h>
#include <math.h> // required for round()
#include <stdlib.h>
#include <string.h>

// #include "pico/stdio.h"
#include "load_cell.h"
#include "pico-scale/extern/hx711-pico-c/include/common.h"
#include "pico-scale/include/scale.h"
#include "pico-scale/include/hx711_scale_adaptor.h"
#include "pico-scale/extern/hx711-pico-c/include/hx711_reader.pio.h"


size_t getchars(char* arr, const size_t len)
{
    memset(arr, 0, len);
    size_t i = 0;
    for(; i < len;) {
        int c = getchar();
        if(c == 0) {
            continue;
        }
        else if(c == '\r' || c == '\n') {
            putchar('\n');
            break;
        }
        putchar(c); //echo
        arr[i] = (char)c;
        ++i;
    }
    return i + 1;
}

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
    
    hx711_wait_settle(hx711_rate_10);
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

int Load_cell::calibrate()
{
    init();
    hx711_scale_adaptor_t hxsa = {0};

    char buff[32];
    char unit[10];
    double knownWeight;
    int32_t zeroValue;
    double raw;
    double refUnitFloat;
    int32_t refUnit;

    scale_t sc;
    scale_options_t opt = SCALE_DEFAULT_OPTIONS;
    scale_options_get_default(&opt);

    const size_t valbufflen = 1000;
    int32_t valbuff[valbufflen];
    opt.buffer = valbuff;
    opt.bufflen = valbufflen;

    start();

    hx711_scale_adaptor_init(&hxsa, &hx);

    scale_init(&sc, hx711_scale_adaptor_get_base(&hxsa), mass_ug, 1, 0);

    // //wait for serial connection
    // while(!tud_cdc_connected()) {
    //     sleep_ms(10);
    // }

    //clear the screen
    printf("\x1B[2J\x1B[H");

    printf(
        "========================================\n\
        HX711 Calibration\n\
        ========================================\n\
        \n\
        Find an object you know the weight of. If you can't find anything, \n\
        try searching Google for your phone's specifications to find its weight. \n\
        You can then use your phone to calibrate your scale.\n\
        \n"
    );

    printf("1. Enter the unit you want to measure the object in (eg. g, kg, lb, oz): ");
    getchars(unit, sizeof(unit));

    printf("\n2. Enter the weight of the object in the unit you chose (eg. \
        if you chose 'g', enter the weight of the object in grams): ");
    getchars(buff, sizeof(buff));
    knownWeight = atof(buff);

    printf("\n3. Enter the number of samples to take from the HX711 chip (eg. 1000): ");
    getchars(buff, sizeof(buff));
    opt.samples = (size_t)atol(buff);

    printf("\n4. Remove all objects from the scale and then press enter.");
    getchar();
    printf("\nWorking...");

    if(!scale_read(&sc, &raw, &opt)) {
        printf("ERROR: failed to read from scale");
        return EXIT_FAILURE;
    }

    zeroValue = (int32_t)round(raw);

    printf("\n\n5. Place object on the scale and then press enter.");
    getchar();
    printf("\nWorking...");

    if(!scale_read(&sc, &raw, &opt)) {
        printf("ERROR: failed to read from scale");
    }

    refUnitFloat = (raw - zeroValue) / knownWeight;
    refUnit = (int32_t)round(refUnitFloat);
    hx711_close(&hx);

    if(refUnit == 0) {
        refUnit = 1;
    }

    //cppcheck-suppress invalidPrintfArgType_sint
    printf("\
        \n\n\
        Known weight (your object): %f %s\n\
        Raw value over %zu samples: %li\n\
        \n\
        -> REFERENCE UNIT: %li\n\
        -> OFFSET VALUE: %li\n\
        \n\
        You can provide these values to the scale_init() function. For example: \n\
        \n\
        scale_init(&sc, &hx, /* your chosen mass_unit_t */, %li, %li);\
        \n",
        knownWeight, unit,
        opt.samples, (int32_t)raw,
        refUnit,
        zeroValue,
        refUnit, zeroValue
    );

    getchar();

    return EXIT_SUCCESS;
}