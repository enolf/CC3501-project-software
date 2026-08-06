cmake_minimum_required(VERSION 3.13)


# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")



# Detect if the active kit is an ARM cross-compiler
if(CMAKE_CXX_COMPILER MATCHES "arm-none-eabi")
    message(STATUS "Detected that the current kit is a cross-compiler.")
    set(CrossCompiling 1)
else()
    message(STATUS "Detected that the current kit is a host compiler.")
endif()

# Detect if the active kit is an ARM cross-compiler
if(CrossCompiling)
    # Yes, build for the RP2040

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    # Specify an increased oscillator startup delay to work around a potential hardware issue where the 
    # clock is slow to boot.
    add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

    # Pull in Raspberry Pi Pico SDK (must be before project)
    include(pico_sdk_import.cmake)

    project(cc3501-labs C CXX ASM)
    
    # Initialise the Raspberry Pi Pico SDK
    pico_sdk_init()

    add_executable(labs)

    add_subdirectory(lib/lvgl)
    add_subdirectory(lib/pico-scale)

    # Every source file this project wrote. Named as a list so the warning
    # settings below can be applied to exactly these files and nothing else.
    set(PROJECT_SOURCES
        src/main.cpp
        src/drivers/logging/logging.cpp
        src/peripherals/events/events.cpp
        src/drivers/DigitalSwitch/DigitalSwitch.cpp
        src/peripherals/switches/switches.cpp
        src/drivers/sd_card/sd_card.cpp
        src/drivers/sd_card/sd_diskio.cpp
        src/peripherals/sd_log/sd_log.cpp
        src/drivers/ili9341/ili9341.c
        src/peripherals/tft_display/tft_display.cpp
        src/drivers/DS18B20/DS18B20.cpp
        src/peripherals/sensor_health/sensor_health.cpp
        src/drivers/mfrc522/mfrc522.cpp
        src/peripherals/access_control/access_control.cpp
        # src/peripherals/load_cell/load_cell.cpp
        #   Superseded by src/drivers/mass_sensor/. Deliberately NOT built: its
        #   init() points the HX711's sample buffer at a stack-local array that
        #   dies when init() returns, so every later read writes through a
        #   dangling pointer. Kept on disk as the original bring-up reference.
        src/peripherals/basket/basket.cpp
        # src/peripherals/camera_side/inventory.cpp
        #   Superseded by peripherals/catalogue (the price table) and
        #   peripherals/basket (counts, diffing and the payment gate). Kept on
        #   disk for its dashboard price-override sketch, which the Pi link will
        #   want later, but out of the build so there is only ONE definition of
        #   what a drink costs.
        src/drivers/ili9341/xpt2046.c
        src/drivers/mass_sensor/mass_sensor.cpp
        src/peripherals/coin_acceptor/coin_acceptor.cpp
        src/peripherals/sim_input/sim_input.cpp
        src/peripherals/checkout/checkout.cpp
        src/peripherals/scale_task/scale_task.cpp
        src/peripherals/nfc_task/nfc_task.cpp
        src/peripherals/temperature_task/temperature_task.cpp
        # Compiled although nothing calls it until the serial backend arrives at
        # stage 12. Building it here means the ARM toolchain and this project's
        # warning flags check it too, rather than only the host compiler that
        # runs the tests. The linker discards it while it is unused.
        src/peripherals/pi_link/frame.cpp
    )

    # ===== Keyboard stand-ins for hardware =====
    #
    # See src/sim_config.h for the full rationale. In short: real hardware is
    # always compiled and always attempted, and these decide whether the
    # keyboard stand-ins exist alongside it. A bench build has them all; a build
    # that goes in the fridge has none, because the keys can fake a payment.
    #
    #   Bench (default):  cmake ..
    #   Deployment:       cmake .. -DSIM_ALL=OFF
    #   Selective:        cmake .. -DSIM_NFC=ON -DSIM_DOOR=OFF ...
    option(SIM_ALL "Default for every keyboard stand-in" ON)
    option(SIM_DOOR  "Keys o/c stand in for the door switch"  ${SIM_ALL})
    option(SIM_NFC   "Key n stands in for a card tap"         ${SIM_ALL})
    option(SIM_COINS "Keys 5/6/7 stand in for the coin scale" ${SIM_ALL})
    option(SIM_TOUCH "Keys , . / stand in for the touchscreen" ${SIM_ALL})

    foreach(sim IN ITEMS SIM_DOOR SIM_NFC SIM_COINS SIM_TOUCH)
        if(${sim})
            add_compile_definitions(${sim}=1)
        else()
            add_compile_definitions(${sim}=0)
        endif()
    endforeach()
    message(STATUS "Stand-ins: door=${SIM_DOOR} nfc=${SIM_NFC} "
                   "coins=${SIM_COINS} touch=${SIM_TOUCH}")

    # =========================================================================
    #  IDLE SCREEN  --  FLIP THE LINE BELOW
    #
    #      ON   show assets/logo.png while the fridge is idle
    #      OFF  plain black idle screen
    #
    #  Then reconfigure and rebuild. Nothing else needs changing, and nothing
    #  changes at run time: this is decided when the firmware is compiled.
    #
    #  A plain set() rather than option() on purpose. option() writes the value
    #  into CMake's cache the first time it configures, and editing this file
    #  afterwards would then appear to do nothing at all until the cache was
    #  deleted. This way the file is always the answer.
    #
    #  OFF is the better choice for a fridge left running: the terminal sits on
    #  this screen almost all its life, so black avoids burn-in, draws less
    #  power, and reads unmistakably as "nothing in progress" rather than as a
    #  transaction that has got stuck. ON is for demonstrations and for looking
    #  presentable in the kitchen.
    # =========================================================================
    set(IDLE_LOGO OFF)
    if(IDLE_LOGO)
        if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/assets/idle_logo.c)
            message(FATAL_ERROR
                "IDLE_LOGO=ON but src/assets/idle_logo.c does not exist. "
                "Generate it from the artwork first:\n"
                "    pip install pillow\n"
                "    cd assets && python convert_logo.py logo.png")
        endif()
        add_compile_definitions(IDLE_SHOW_LOGO=1)
        # The generated image is only compiled when it is actually wanted, so a
        # black-screen build does not carry tens of kilobytes of unused artwork.
        list(APPEND PROJECT_SOURCES src/assets/idle_logo.c)
    else()
        add_compile_definitions(IDLE_SHOW_LOGO=0)
    endif()
    message(STATUS "Idle screen: ${IDLE_LOGO}")

    # ===== Which Raspberry Pi link is compiled =====
    #
    # The single most important switch in this build. Everything the Pi does
    # sits behind src/peripherals/pi_link/pi_link.h, and exactly ONE
    # implementation of that header is compiled:
    #
    #   sim     - fabricates inventory and Square replies from the keyboard,
    #             so the whole system runs with no Pi, no camera and no network
    #   serial  - the real transport (stage 12, not written yet)
    #
    # Only one is built, so the unused one costs no flash. Swapping the real Pi
    # in later is this one word.
    set(PI_LINK_BACKEND "sim" CACHE STRING "Pi link implementation: sim or serial")
    set_property(CACHE PI_LINK_BACKEND PROPERTY STRINGS sim serial)

    if(PI_LINK_BACKEND STREQUAL "sim")
        list(APPEND PROJECT_SOURCES src/peripherals/pi_link/pi_link_sim.cpp)
        add_compile_definitions(PI_LINK_SERIAL=0)
    elseif(PI_LINK_BACKEND STREQUAL "serial")
        list(APPEND PROJECT_SOURCES src/peripherals/pi_link/pi_link_serial.cpp)
        # Tells sim_input to stop reading stdin. The serial backend owns the
        # byte stream; two readers would split it and every frame would arrive
        # with characters missing.
        add_compile_definitions(PI_LINK_SERIAL=1)
    else()
        message(FATAL_ERROR
            "PI_LINK_BACKEND must be 'sim' or 'serial', got '${PI_LINK_BACKEND}'.")
    endif()
    message(STATUS "Pi link backend: ${PI_LINK_BACKEND}")

    target_sources(labs PUBLIC ${PROJECT_SOURCES})

    # FatFs (ChaN, R0.16) — vendored third-party source, NOT in PROJECT_SOURCES
    # so the warning flags below do not apply to it. Only ff.c is needed:
    # ffunicode.c is for long filenames (FF_USE_LFN is 0, we use 8.3 names) and
    # ffsystem.c is for reentrancy and dynamic allocation, both disabled in
    # lib/fatfs/ffconf.h. The disk_* functions FatFs expects are implemented by
    # src/drivers/sd_card/sd_diskio.cpp, which IS our code.
    target_sources(labs PUBLIC lib/fatfs/ff.c)
    target_include_directories(labs SYSTEM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lib/fatfs)

    # Warnings on, for OUR sources only.
    #
    # The Pico SDK enables no warning flags by default, so the build was
    # reporting "0 warnings" only because nothing had been asked to look.
    #
    # Applied per source file rather than with target_compile_options(labs
    # PRIVATE ...), which looks like the obvious choice but is wrong here:
    # pico-scale is an INTERFACE library, so ITS sources are compiled into the
    # labs target too and would inherit the flags. That produced 67 warnings in
    # vendored code this project does not police (CLAUDE.md section 0) and
    # cannot fix without editing a submodule.
    #
    # KNOWN BASELINE: 2 warnings, both third-party, both from
    # lib/pico-scale/extern/hx711-pico-c/include/hx711_multi.h reporting
    # `static` functions declared but never defined, surfaced through
    # mass_sensor.cpp. The vendored include paths are already marked SYSTEM
    # below, but GCC raises -Wunused-function at end of translation unit rather
    # than at the header, so -isystem does not suppress it. Silencing it would
    # mean -Wno-unused-function on mass_sensor.cpp, which would also hide
    # genuinely unused statics in OUR half of that file — not worth it.
    #
    # So: "no new warnings" means this build stays at 2, with 0 from src/.
    set_source_files_properties(${PROJECT_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")


    # SYSTEM on both: these paths hold third-party headers (lvgl, pico-scale and
    # its nested hx711-pico-c). Marking them system tells GCC to suppress
    # warnings originating inside them, so -Wall -Wextra below reports on code
    # this project actually wrote and can fix. Without this the vendored HX711
    # headers alone produce 89 warnings that no one here should be acting on
    # (CLAUDE.md section 0).
    target_include_directories(lvgl SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)

    target_include_directories(pico-scale SYSTEM INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/lib/pico-scale/include  # Or whatever folder contains their .h files
    )

    target_include_directories(labs
        PUBLIC 
        ${CMAKE_CURRENT_LIST_DIR}/src/
        # ${CMAKE_CURRENT_LIST_DIR}/lib/
        ${CMAKE_CURRENT_LIST_DIR}/src/peripherals/
    )

    # NOTE: do not be tempted to move the warning flags above into
    # CMAKE_C_FLAGS / CMAKE_CXX_FLAGS. Setting those globally displaces the
    # architecture flags the SDK relies on (-mcpu=cortex-m0plus -mthumb), and
    # the SDK's own C sources then fail to assemble with errors about the
    # processor not supporting `cpsid i`.

    pico_set_program_name(labs "cc3501-labs")
    pico_set_program_version(labs "0.2")

    # select USB CDC for standard IO, so the serial monitor works over the same
    # cable used to flash the board (the coin detector test in main.cpp is
    # driven interactively over this). Swap these back to use the UART header.
    pico_enable_stdio_uart(labs 0)
    pico_enable_stdio_usb(labs 1)

    # Add the standard library to the build
    target_link_libraries(labs
        pico_stdlib
        hardware_spi
        hardware_i2c
        hardware_adc    # RP2040 internal die temperature sensor (ADC input 4)
        hardware_dma
        hardware_pio
        hardware_interp
        hardware_timer
        hardware_watchdog
        hardware_clocks
        hardware_pwm
        
        # user added
        lvgl
        pico_platform
        pico_sync
        pico_time
        hardware_gpio
        hardware_irq
        pico-scale
    )

    pico_add_extra_outputs(labs)

else()

    # The old native branch built a test harness against the stubs in
    # tests/mocks/. It has been removed: it referenced src/tasks/sensor_health.cpp,
    # a path that no longer exists (that file lives in src/peripherals/), so the
    # target had not built for some time. The mocks are dev-board leftovers
    # rather than part of this project — see documentation.md section 4.7.
    #
    # Off-hardware testing is being reintroduced as a separate, self-contained
    # target under tests/host/, which compiles only the modules that have no
    # hardware dependency (coin classification, basket arithmetic, frame
    # parsing) and needs no Pico SDK stubs at all.
    message(FATAL_ERROR
        "This project builds only for the RP2040. The active CMake kit is a host "
        "compiler; select the ARM cross-compiler kit (arm-none-eabi). For "
        "off-hardware unit tests, configure tests/host/ as its own project.")

endif()

target_compile_definitions(labs
    PUBLIC
    LOG_DRIVER_STYLE=${LogDriverImplementation}
)
