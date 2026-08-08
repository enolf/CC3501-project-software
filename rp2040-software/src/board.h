#pragma once

// board.h — ALL hardware connectivity lives here.
//
// This project targets a custom PCB, and the board may go through several
// iterations. When the wiring changes (a pin moves, a power option changes,
// a bus address changes), change these definitions and nothing else — no
// driver or application file should ever contain a raw pin number, bus
// address, or wiring assumption.

// Some definitions below name an SDK peripheral instance (i2c1, spi0) rather
// than a plain number, and those names are macros the SDK headers define. They
// are included here so this header stands on its own: a file that includes
// board.h gets working definitions no matter what order its own includes are
// in. Without this it only ever compiled by luck — every current consumer
// happens to include the SDK header first, and the first one that did not
// would fail with an unhelpful "'i2c1' was not declared" pointing at board.h
// rather than at the missing include.
//
// HX711_RATE below has the same shape but is deliberately NOT covered: its
// enum lives in a vendored pico-scale header, and including that here would
// drag third-party code into every file that includes board.h — including the
// plain-C display driver, which has no business seeing it. Only mass_sensor.cpp
// uses HX711_RATE, and it includes that header itself.
#include "hardware/i2c.h"
#include "hardware/spi.h"

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
// CONFIRMED against the PCB: the PiicoDev connector is wired to GP2/GP3.
//
// Worth knowing if these ever move: GP6/GP7 are the *other* pin pair i2c1 can
// use, and this board has already spent both of them (limit switch and DS18B20
// bus). So i2c1 has exactly one free home on this design.
#define RFID_I2C_INSTANCE   i2c1
#define RFID_SDA_PIN        2
#define RFID_SCL_PIN        3

// PiicoDev bus runs at standard 100 kHz. The module has its own pull-ups.
#define RFID_I2C_BAUDRATE   100000

// Module I2C address. Factory default is 0x2C, set by the module's ASW address
// switches; the range is 0x2C-0x2F, so up to four modules can share one bus
// (per the PiicoDev README). Change this if the switches on your board differ.
#define RFID_I2C_ADDR       0x2C


// Limit switch (door sensor) and user button.
// Both are wired identically: one side of the switch to 3V3, the other side
// to the GPIO, with an external 100 kOhm resistor + 100 nF capacitor from
// the GPIO node to ground (RC debounce filter, and it also defines the idle
// level when the switch is open -- no internal pull needed).
//
// Both switches are Normally Open (NO), so the GPIO reads LOW when idle
// (open circuit, pulled to GND through the 100k) and HIGH when actuated
// (switch closes the path to 3V3) -- hence ACTIVE_HIGH = 1 for both. A
// Normally-Closed switch wired the same physical way would read the
// opposite way (HIGH idle, LOW when actuated), so its ACTIVE_HIGH would be 0.
#define LIMIT_SWITCH_PIN 6
#define LIMIT_SWITCH_ACTIVE_HIGH 1
#define USER_SWITCH_PIN 15
#define USER_SWITCH_ACTIVE_HIGH 1


// TFT DISPLAY

// model: ILI9341 with Touch (XPT2046) and SD Card
//
// The display and the touch controller are two separate chips sharing one SPI
// bus, each with its own chip-select. They run at very different clock rates,
// so the driver re-baudrates the bus around every touch transaction.

// --- Wiring ---
#define DISPLAY_SPI_INSTANCE spi0
#define DISPLAY_MISO_PIN     16
#define DISPLAY_CS_PIN       17
#define DISPLAY_SCK_PIN      18
#define DISPLAY_MOSI_PIN     19
#define DISPLAY_DC_PIN       20   // low = command, high = data
#define DISPLAY_RST_PIN      21

// Backlight switch. On this board it is a P-channel MOSFET, so a LOW gate
// turns the backlight ON. Set to 0 for a board revision that drives the
// backlight through an active-high stage (e.g. an NPN transistor), which is
// what the breadboard prototype used.
#define DISPLAY_BL_PIN         22
#define DISPLAY_BL_ACTIVE_LOW  1

// Touch controller (XPT2046) — same SPI bus, separate chip select.
// PENIRQ goes LOW while the panel is being touched.
#define TOUCH_CS_PIN   24
#define TOUCH_IRQ_PIN  25

// Bus rates. The ILI9341 is happy at 30 MHz, but the XPT2046 is specified far
// slower and returns nonsense if clocked at display speed.
#define DISPLAY_SPI_BAUDRATE 30000000
#define TOUCH_SPI_BAUDRATE   2000000

// --- microSD socket on the TFT module ---
//
// THIRD DEVICE ON THE SAME SPI BUS. The card's SCK/MOSI/MISO are the display's
// (GP18/GP19/GP16); only the chip select is its own. Every device on this bus
// therefore has to leave the other two deselected and put the bus rate back
// where it found it — the touch driver already works this way.
//
// A card that fails to release MISO when deselected will corrupt XPT2046 touch
// readings, because these display modules frequently omit the buffer that
// guarantees it. If touch starts misbehaving after the card is fitted, suspect
// this before anything else.
#define SD_CS_PIN 23

// The card must be clocked between 100 and 400 kHz until it has finished its
// initialisation handshake; it is permitted to ignore anything faster. Once
// initialised it will take many MHz, but this bus is shared with a 30 MHz
// display over unknown trace lengths, so the working rate is kept modest.
#define SD_SPI_INIT_BAUDRATE   400000
#define SD_SPI_BAUDRATE      12000000

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
    // VERIFIED against the on-screen buttons, not just against printed
    // coordinates. X was previously false, which mirrored the touch layer: a
    // press on the left-hand CASH button registered on the right-hand ONLINE
    // one and vice versa.
    //
    // Worth knowing for the other orientations below: CALIBRATE_TOUCH_MODE does
    // NOT catch this. It prints raw and mapped values, both of which look
    // perfectly reasonable when an axis is mirrored — the numbers are in range
    // and move smoothly. Only a target with a known left and right reveals it.
    // Check any change here by pressing the CASH and ONLINE buttons, not by
    // reading coordinates.
    #define TOUCH_INVERT_X  true
    #define TOUCH_INVERT_Y  true

#elif ACTIVE_DISPLAY_ORIENTATION == ORIENTATION_LANDSCAPE_VCC_DOWN
    #define DISP_H_V_CONF   0x68
    #define DISP_HOR_RES    320
    #define DISP_VER_RES    240
    #define TOUCH_SWAP_XY   true
    // A 180 degree rotation inverts both axes relative to VCC_UP, so these
    // follow from the corrected VCC_UP values above. DERIVED, NOT MEASURED:
    // this orientation has not been tested on hardware. If it is ever used,
    // confirm it with the CASH/ONLINE buttons before trusting it.
    #define TOUCH_INVERT_X  false
    #define TOUCH_INVERT_Y  false

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
// These are the only HX711 pin definitions in the firmware. An earlier
// bare-Pico test rig used GP14/GP15; that rig's driver has been deleted, so
// there is no second set to be confused with.
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

// Load cell calibration: raw HX711 counts per gram
// (TAL221 500 g, originally calibrated against a 50 g mass).
//
// This is the ONLY calibration figure the coin detector needs. It works
// entirely in differences from a baseline captured at run time by tare(), so
// any zero offset cancels out -- no stored zero has to be correct, or even
// current, for coin detection to work.
// Re-derived on this board from known coin masses, replacing the 2945 figure
// inherited from an earlier bare-Pico test rig. That value read 8.5% high,
// which is what made a 9.00 g $1 coin appear to weigh 9.80 g.
//
//   4 x $2: reported 28.6 g for a true 26.40 g  ->  28.6 * 2945 / 26.40 = 3190.4
//   4 x $1: reported 39.1 g for a true 36.00 g  ->  39.1 * 2945 / 36.00 = 3198.6
//
// The two agree to 0.13% despite coming from different coins, which is what
// identifies this as a pure scale-factor error rather than coin-to-coin
// variation. Four coins at a time rather than one, so wear on any single coin
// averages out.
//
// TO REDO THIS: press 't' to tare, then 'g' for the raw dump, place four coins
// of one denomination, and divide reported grams by true grams to get the
// correction factor.
#define LOADCELL_COUNTS_PER_GRAM 3194.5
