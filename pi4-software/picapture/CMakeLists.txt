cmake_minimum_required(VERSION 3.12)
project(PiCapture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenCV 4 specifically. 5.0.0 removed the Moments class this depends on, and
# the failure is a wall of template errors rather than anything that names the
# problem — so it is pinned here where the message can say so.
#
# QUIET rather than REQUIRED, and the target below is conditional, because the
# tests must configure on a machine that has no OpenCV. They were advertised as
# running "on any laptop" while a REQUIRED find_package above them made that
# impossible: `cmake -S . -B build` failed outright, so the off-hardware test
# command in the README could not be run anywhere except the Pi.
#
# A missing OpenCV is still said out loud below, so a Pi that is meant to build
# the real thing cannot quietly produce a tests-only build.
find_package(OpenCV 4 QUIET)

if(OpenCV_FOUND)
    add_executable(PiCapture
        src/main.cpp
        src/vision_config.cpp
    )
    target_link_libraries(PiCapture ${OpenCV_LIBS})
    target_include_directories(PiCapture PRIVATE ${OpenCV_INCLUDE_DIRS} src)
    target_compile_options(PiCapture PRIVATE -O2 -Wall -Wextra)
    message(STATUS "OpenCV ${OpenCV_VERSION} found — building PiCapture and config_tests")
else()
    message(STATUS
        "OpenCV 4 NOT found — building config_tests ONLY, without PiCapture.\n"
        "   This is the expected result on a development laptop.\n"
        "   On the Raspberry Pi this is a fault: install OpenCV with\n"
        "       sudo apt install libopencv-dev\n"
        "   and reconfigure, or the camera has no program to run.")
endif()

# --- Off-camera tests -------------------------------------------------------
#
# vision_config has no OpenCV dependency, deliberately: the parsing and
# round-tripping are the parts most likely to be wrong and the parts least
# convenient to debug standing at a fridge. They build and run on any laptop.
#
#   cmake -S . -B build && cmake --build build && ./build/config_tests
add_executable(config_tests
    tests/test_vision_config.cpp
    src/vision_config.cpp
)
target_include_directories(config_tests PRIVATE src)
target_compile_options(config_tests PRIVATE -Wall -Wextra)
