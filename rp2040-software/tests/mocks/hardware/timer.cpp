#include <thread>
#include <chrono>

#include "hardware/timer.h"

void busy_wait_us_32(uint32_t delay_us)
{
    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
}
