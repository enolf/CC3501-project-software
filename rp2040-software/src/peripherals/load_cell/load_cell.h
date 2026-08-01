#pragma once

// ============================================================================
// NOT BUILT. SUPERSEDED. DO NOT INCLUDE THIS FILE IN NEW CODE.
//
// This was the original HX711 bring-up example, written against a bare Pico
// test rig. It has been removed from CMakeLists.txt for two reasons:
//
//   1. init() points the HX711's sample buffer at a stack-local array that
//      goes out of scope when init() returns. Every later read then writes
//      through a dangling pointer.
//   2. The pin numbers below describe the OLD TEST RIG, not this PCB. The
//      real wiring is HX711_DATA_PIN / HX711_CLK_PIN in board.h.
//
// Use src/drivers/mass_sensor/ instead. Kept on disk only as a reference for
// the calibration routine.
// ============================================================================

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