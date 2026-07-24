#pragma once

#define HX711_CLK_GPIO_PIN 14
#define HX711_DATA_GPIO_PIN 15
#define UNIT mass_g

// TAL221 (max 500g model) calibrated to 50g
#define REFUNIT 2945
#define OFFSET -63750

class Load_cell
{
    public:
        /// @brief Initialise the load cell to be used as a scale. Default uses a TAL221 (max 500g model) calibrated to 50g
        void init();

        /// @brief Return mass on load cell as double
        /// @return mass
        double get_mass();

        /// @brief Run load cell and print print in serial 
        void measure();

        void stop();

        /// @brief Calibrate load cell. Follow instructions in serial
        int calibrate();
};