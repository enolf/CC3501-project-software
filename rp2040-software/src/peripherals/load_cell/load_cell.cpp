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

#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "load_cell.h"
#include "pico-scale/extern/hx711-pico-c/include/common.h"
#include "pico-scale/include/scale.h"
#include "pico-scale/include/hx711_scale_adaptor.h"
#include "pico-scale/extern/hx711-pico-c/include/hx711_reader.pio.h"

hx711_t hx = {0};
hx711_config_t hxcfg = {0};
hx711_scale_adaptor_t hxsa = {0};

scale_t sc;
scale_options_t opt;

mass_t mass;
mass_t max;
mass_t min;

char str[MASS_TO_STRING_BUFF_SIZE];

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

void init_hx711()
{
    hx711_get_default_config(&hxcfg);

    // Set GPIO pins DATA and CLOCK 
    hxcfg.clock_pin = HX711_CLK_GPIO_PIN;
    hxcfg.data_pin = HX711_DATA_GPIO_PIN;

    // Initialize the hardware using the config struct
    hx711_init(&hx, &hxcfg);

    //Power up the hx711 and set gain on chip
    hx711_power_up(&hx, hx711_gain_128);
    hx711_wait_settle(hx711_rate_10); // alternatively use hx711_rate_80

    // provide a pointer to the hx711 to the adaptor
    hx711_scale_adaptor_init(&hxsa, &hx);
}

void Load_cell::init()
{    
    // scale_t sc; // {0};
    // scale_options_t opt = {0};
    scale_options_get_default(&opt);
    
    // provide a read buffer for the scale
    const size_t valbufflen = 1000;
    int32_t valbuff[valbufflen];
    opt.buffer = valbuff;
    opt.bufflen = valbufflen;
    
    // initialise hx711
    init_hx711();

    // initialise the scale
    scale_init(
        &sc,
        hx711_scale_adaptor_get_base(&hxsa),
        UNIT,
        REFUNIT,
        OFFSET
    );

    //spend 10 seconds obtaining as many samples as
    //possible to zero (aka. tare) the scale. The max
    //number of samples will be limited to the size of
    //the buffer allocated above
    opt.strat = strategy_type_time;
    opt.timeout = 10000000;

     if(scale_zero(&sc, &opt)) {
        printf("Scale zeroed successfully\n");
    }
    else {
        printf("Scale failed to zero\n");
    }

    //change to spending 250 milliseconds obtaining
    opt.timeout = 2500000;

    mass_init(&max, mass_g, 0);
    mass_init(&min, mass_g, 0);
}

double Load_cell::get_mass()
{
    double n; //value
    mass_get_value(&mass, &n);

    return n;
} 

void Load_cell::measure()
{
    memset(str, 0, MASS_TO_STRING_BUFF_SIZE);

    //obtain a mass from the scale
    if(scale_weight(&sc, &mass, &opt)) {

        //check if the newly obtained mass
        //is less than the existing minimum mass
        if(mass_lt(&mass, &min)) {
            min = mass;
        }

        //check if the newly obtained mass
        //is greater than the existing maximum mass
        if(mass_gt(&mass, &max)) {
            max = mass;
        }

        //display the newly obtained mass...
        mass_to_string(&mass, str);
        printf("%s", str);

        //...the current minimum mass...
        mass_to_string(&min, str);
        printf(" min: %s", str);

        //...and the current maximum mass
        mass_to_string(&max, str);
        printf(" max: %s\n", str);

    }
    else {
        printf("Failed to read weight\n");
    }
}

void Load_cell::stop()
{
    // Stop communication with HX711
    hx711_close(&hx);
}

int Load_cell::calibrate()
{
    init_hx711();

    char buff[32];
    char unit[10];
    double knownWeight;
    int32_t zeroValue;
    double raw;
    double refUnitFloat;
    int32_t refUnit;

    scale_options_t opt = SCALE_DEFAULT_OPTIONS;
    scale_options_get_default(&opt);

    const size_t valbufflen = 1000;
    int32_t valbuff[valbufflen];
    opt.buffer = valbuff;
    opt.bufflen = valbufflen;

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