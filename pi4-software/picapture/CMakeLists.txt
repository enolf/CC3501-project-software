cmake_minimum_required(VERSION 3.12)
project(PiCapture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenCV 4 specifically. 5.0.0 removed the Moments class this depends on, and
# the failure is a wall of template errors rather than anything that names the
# problem — so it is pinned here where the message can say so.
find_package(OpenCV 4 REQUIRED)

add_executable(PiCapture
    src/main.cpp
    src/vision_config.cpp
)
target_link_libraries(PiCapture ${OpenCV_LIBS})
target_include_directories(PiCapture PRIVATE ${OpenCV_INCLUDE_DIRS} src)
target_compile_options(PiCapture PRIVATE -O2 -Wall -Wextra)

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
