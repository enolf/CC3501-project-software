// Off-hardware unit tests.
//
// Runs on the development PC with the system compiler. Only modules with no
// hardware dependency are compiled in — the arithmetic that decides what the
// customer took, what it costs, and whether they have paid. Those are exactly
// the parts where a mistake is expensive and standing at a fridge feeding it
// coins is a slow way to find out.
//
// This deliberately does NOT revive tests/mocks/, the old Pico SDK stubs. That
// approach needs a fake for every SDK function the code touches and breaks
// whenever the firmware grows a new dependency. Testing only the hardware-free
// modules needs no fakes at all.

#include "test_support.h"

void test_catalogue();
void test_diff();
void test_mass_gate();
void test_coin_acceptor();
void test_frame_build();
void test_frame_round_trip();
void test_frame_rejection();
void test_frame_stream();
void test_frame_payload_fields();

int main()
{
    printf("=========================================\n");
    printf("Smart Fridge - off-hardware unit tests\n");
    printf("=========================================\n");

    test_catalogue();
    test_diff();
    test_mass_gate();
    test_coin_acceptor();
    test_frame_build();
    test_frame_round_trip();
    test_frame_rejection();
    test_frame_stream();
    test_frame_payload_fields();

    return testing::summary();
}
