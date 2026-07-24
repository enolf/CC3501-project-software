#pragma once

#include <stdint.h>

// Busy-wait delay (approximated with a thread sleep in the native harness)
void busy_wait_us_32(uint32_t delay_us);
