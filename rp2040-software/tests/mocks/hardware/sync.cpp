#include "hardware/sync.h"

// There are no interrupts in the native test harness, so these are no-ops.

uint32_t save_and_disable_interrupts()
{
    return 0;
}

void restore_interrupts(uint32_t status)
{
    (void)status;
}
