cmake_minimum_required(VERSION 3.13)

# Off-hardware unit tests.
#
# A completely separate project from the firmware, configured on its own and
# built with the host compiler:
#
#   cmake -S tests/host -B build-host
#   cmake --build build-host
#   ./build-host/fridge_tests
#
# Kept separate rather than added to the firmware's CMakeLists on purpose. That
# file configures the Pico SDK and cross-compiles for the RP2040; trying to
# serve both from one place is what left the old native branch broken and
# pointing at a file that no longer existed.
#
# Only hardware-free sources may be listed here. If a test ever needs a Pico SDK
# call, that is a sign the logic and the hardware access want separating in the
# firmware — not a sign that this project needs SDK stubs.

project(fridge_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FIRMWARE_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)

add_executable(fridge_tests
    main.cpp
    test_basket.cpp
    test_coin_acceptor.cpp
    test_frame.cpp

    # Modules under test. Every one of these is pure arithmetic or string
    # handling, with no hardware dependency at all.
    ${FIRMWARE_SRC}/peripherals/basket/basket.cpp
    ${FIRMWARE_SRC}/peripherals/coin_acceptor/coin_acceptor.cpp
    ${FIRMWARE_SRC}/peripherals/pi_link/frame.cpp
    # catalogue is header-only (a constexpr table), so there is nothing to
    # compile for it beyond the include path below.
)

target_include_directories(fridge_tests PRIVATE
    ${FIRMWARE_SRC}
    ${CMAKE_CURRENT_LIST_DIR}
)

# Same warning settings as the firmware, so a problem is caught in whichever
# build runs first.
target_compile_options(fridge_tests PRIVATE -Wall -Wextra)
