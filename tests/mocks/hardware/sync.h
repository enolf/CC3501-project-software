#pragma once

#include <stdint.h>

// Interrupt control (no-ops in the native test harness)
uint32_t save_and_disable_interrupts();
void restore_interrupts(uint32_t status);
