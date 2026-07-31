#pragma once

#ifndef BOARD_H
#define BOARD_H

// board.h — ALL hardware connectivity lives here.
//
// This project targets a custom PCB, and the board may go through several
// iterations. When the wiring changes (a pin moves, a power option changes,
// a bus address changes), change these definitions and nothing else — no
// driver or application file should ever contain a raw pin number, bus
// address, or wiring assumption.

// DS18B20 1-Wire temperature sensor bus.
// Every DS18B20 shares this single data pin. The data line MUST have an
// external 4.7 kOhm pull-up resistor to 3V3 — the RP2040's internal pull-up
// (~50 kOhm) is too weak to be relied on, especially with multiple sensors.
#define DS18B20_BUS_PIN 7

// How the DS18B20 sensors are powered on this board revision:
//   0 = external supply (sensor VDD wired to 3V3)
//   1 = parasite power  (sensor VDD wired to GND; power drawn from the data
//       line, which is driven high hard during conversions)
#define DS18B20_PARASITE_POWER 0

// Strong pull-up FET for parasite-power conversions (DS18B20 datasheet
// Figure 6): an AO3401A P-channel FET from 3V3 to the data line, its gate
// driven by this GPIO. The driver holds the FET off at all times except
// during a parasite-power conversion. NOTE: the PCB should include a pull-up
// resistor on the gate so the FET stays off while the RP2040 boots (a
// floating gate can turn a P-FET on and jam the bus high).
#define DS18B20_PULLUP_FET_PIN 8
// AO3401A is P-channel: gate low = FET on. Set to 0 if a future board
// revision drives the FET through an inverting stage.
#define DS18B20_PULLUP_FET_ACTIVE_LOW 1

// --- PiicoDev RFID module (MFRC522 over I2C) ---
// TODO: confirm these match the pins the PiicoDev connector is actually wired to.
// GP2/GP3 are the RP2040 default i2c1 pins — change if your board uses different ones.
#define RFID_I2C_INSTANCE   i2c1
#define RFID_SDA_PIN        2
#define RFID_SCL_PIN        3

// PiicoDev bus runs at standard 100 kHz. The module has its own pull-ups.
#define RFID_I2C_BAUDRATE   100000

// Module I2C address. Factory default is 0x2C, set by the module's ASW address
// switches; the range is 0x2C-0x2F, so up to four modules can share one bus
// (per the PiicoDev README). Change this if the switches on your board differ.
#define RFID_I2C_ADDR       0x2C


// TFT DISPLAY

// model: ILI9341 with Touch (XPT2046) and SD Card
// Currently allow to configure between the two landscape orientations

// --- Available Orientations ---

#define ORIENTATION_PORTRAIT           0
#define ORIENTATION_LANDSCAPE_VCC_DOWN 1
#define ORIENTATION_LANDSCAPE_VCC_UP   2

// === SET YOUR ACTIVE ORIENTATION HERE ===
#define ACTIVE_DISPLAY_ORIENTATION ORIENTATION_LANDSCAPE_VCC_UP

// --- Cascading Hardware Settings ---
#if ACTIVE_DISPLAY_ORIENTATION == ORIENTATION_LANDSCAPE_VCC_UP
    #define DISP_H_V_CONF   0xA8
    #define DISP_HOR_RES    320
    #define DISP_VER_RES    240
    #define TOUCH_SWAP_XY   true
    #define TOUCH_INVERT_X  false // Adjust these based on your specific touch film
    #define TOUCH_INVERT_Y  true

#elif ACTIVE_DISPLAY_ORIENTATION == ORIENTATION_LANDSCAPE_VCC_DOWN
    #define DISP_H_V_CONF   0x68
    #define DISP_HOR_RES    320
    #define DISP_VER_RES    240
    #define TOUCH_SWAP_XY   true
    #define TOUCH_INVERT_X  true  // Flipped 180 from VCC_UP
    #define TOUCH_INVERT_Y  false // Flipped 180 from VCC_UP

#elif ACTIVE_DISPLAY_ORIENTATION == ORIENTATION_PORTRAIT
    #define DISP_H_V_CONF  0x48  // Standard portrait MADCTL
    #define DISP_HOR_RES       240
    #define DISP_VER_RES      320
    #define TOUCH_SWAP_XY   false
    #define TOUCH_INVERT_X  false
    #define TOUCH_INVERT_Y  false
#endif

// --- Raw Touch Calibration Boundaries ---
#define TOUCH_X_MIN 236
#define TOUCH_X_MAX 1800
#define TOUCH_Y_MIN 261
#define TOUCH_Y_MAX 1880
// Set CALIBRATE_TOUCH_MODE to 1 to see calibration stats in serial
#define CALIBRATE_TOUCH_MODE 0


// --- HX711 load cell amplifier (money box coin scale) ---
//
// A TAL221 500 g beam load cell sits under the money box, feeding an HX711
// 24-bit amplifier which the RP2040 reads over two GPIOs using a PIO state
// machine (see lib/pico-scale/extern/hx711-pico-c).
//
// NOTE: src/peripherals/load_cell/load_cell.h also defines HX711 pins
// (GP14/GP15). Those are from the earlier bare-Pico test rig and do NOT
// describe this board. They are left untouched so the existing Load_cell
// example still builds; all new code uses the definitions below.
#define HX711_DATA_PIN 10
#define HX711_CLK_PIN  11

// The HX711's RATE pin is strapped LOW on this board, giving 10 samples per
// second. The datasheet quotes 400 ms of output settling at 10 Hz, which is
// what makes coin detection take roughly a second per coin: the reading has
// to finish ramping to the new weight before it can be trusted. 10 Hz is the
// *lower noise* of the two modes (narrower bandwidth), so this costs latency,
// not accuracy. If a board revision straps RATE high, change this to
// hx711_rate_80 and the settle timing follows automatically.
#define HX711_RATE hx711_rate_10

// Analog supply (VSUP) is 5 V for stronger bridge excitation; the digital
// supply (DVDD) is 3V3 so DOUT's logic level is safe for the RP2040, which is
// not 5 V tolerant. Recorded here because it is a board wiring fact, even
// though no code reads it.

// Load cell calibration: raw HX711 counts per gram, from the calibration
// routine in load_cell.cpp (TAL221 500 g, calibrated against a 50 g mass).
//
// This is the ONLY calibration figure the coin detector needs. It works
// entirely in differences from a baseline captured at run time, so any zero
// offset cancels out -- the stored OFFSET in load_cell.h does not have to be
// correct, or even current, for coin detection to work.
#define LOADCELL_COUNTS_PER_GRAM 2945.0

#endif // BOARD_H