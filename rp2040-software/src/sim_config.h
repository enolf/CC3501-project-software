#pragma once

// Which keyboard stand-ins are compiled in.
//
// WHY THIS EXISTS
// ---------------
// Every subsystem in this project was built against a simulator before it was
// built against hardware, and that turned out to be worth keeping rather than
// throwing away. It means:
//
//   * the whole customer flow can be demonstrated at a desk, with no fridge,
//     no camera, no coins and no network;
//   * a peripheral that is broken or not yet fitted does not block work on
//     anything else;
//   * when hardware misbehaves, the same flow can be driven from the keyboard
//     to prove the fault is in the device and not in the logic above it.
//
// That last one is the most valuable and the least obvious. A subsystem that
// can be driven two ways is a subsystem whose failures can be localised.
//
// HOW IT WORKS
// ------------
// Real hardware is ALWAYS compiled and ALWAYS attempted. These switches control
// only whether the keyboard stand-ins exist ALONGSIDE it. A task whose hardware
// fails to initialise logs the failure and carries on, so on a bench build the
// keys still drive the flow.
//
// The Pi link is the one exception and is chosen separately, by
// PI_LINK_BACKEND in CMakeLists.txt. That is a genuine either/or — there is one
// serial port and either the real Pi or the simulator owns it — rather than an
// additive stand-in.
//
// TURN THESE OFF FOR A BUILD THAT GOES IN THE FRIDGE
// --------------------------------------------------
// This is not tidiness, it is a security property. With the keys compiled in,
// anyone who can reach the USB socket can fake a paid transaction by typing a
// character. Configure with -DSIM_DOOR=OFF -DSIM_NFC=OFF -DSIM_COINS=OFF
// -DSIM_TOUCH=OFF for anything left unattended, and check the boot banner:
// a build with any of them enabled says so, loudly.

// Defaults are ON so a plain `cmake ..` gives the bench build. CMakeLists.txt
// overrides each one.

#ifndef SIM_DOOR
#define SIM_DOOR 1      ///< keys o / c stand in for the limit switch
#endif

#ifndef SIM_NFC
#define SIM_NFC 1       ///< key n stands in for an approved card tap
#endif

#ifndef SIM_COINS
#define SIM_COINS 1     ///< keys 5 / 6 / 7 stand in for the coin scale
#endif

#ifndef SIM_TOUCH
#define SIM_TOUCH 1     ///< keys , . / stand in for the touchscreen
#endif

/// True if any stand-in is compiled in, i.e. this is not a deployment build.
#define SIM_ANY (SIM_DOOR || SIM_NFC || SIM_COINS || SIM_TOUCH)
