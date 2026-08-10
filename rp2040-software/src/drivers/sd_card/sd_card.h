#pragma once

#include <stdint.h>
#include <stdbool.h>

// microSD card in the TFT module's socket, over SPI.
//
// SCOPE: this is a feasibility spike, not a storage driver. It brings the card
// up, identifies it, and reads one block. That is enough to prove the wiring is
// right and the card answers on a bus it shares with the display and the touch
// controller — the two things worth knowing before committing to a filesystem.
//
// There is deliberately no write path and no filesystem here. Writing a file
// that a laptop can read needs FAT, which is a dependency decision, and writing
// at all needs somewhere safe to do it: an SD card can stall for 100-250 ms
// during an internal erase, which would freeze the display and stall coin
// counting if it happened mid-transaction.
//
// EVERY FUNCTION HERE BLOCKS, some for up to a second. That is why this is
// driven manually from a debug key rather than called from the superloop.

/// What kind of card answered, which decides how addresses are interpreted.
enum class SdCardType : uint8_t {
    None,       ///< Nothing responded
    V1,         ///< SD version 1, byte addressing
    V2,         ///< SD version 2 standard capacity, byte addressing
    V2HC,       ///< SDHC / SDXC, block addressing
};

/// What the card reported about itself.
struct SdCardInfo {
    SdCardType type = SdCardType::None;
    uint64_t   capacity_bytes = 0;
    uint8_t    cid[16] = {};        ///< Card identification register
    uint8_t    csd[16] = {};        ///< Card specific data register
    char       product_name[6] = {}; ///< From the CID, null-terminated
    uint16_t   manufacture_year = 0;
    uint8_t    manufacture_month = 0;
};

/// Configure the chip select and leave the card deselected. Call once at
/// startup, before anything else touches the SPI bus.
void sd_card_init_pins(void);

/// Bring the card up and read its identification registers.
///
/// BLOCKS for up to about a second: the card's initialisation handshake is
/// polled, and the specification allows it to take that long.
///
/// Returns false if no card answers, which is also what an empty socket looks
/// like. `info` is filled in on success.
bool sd_card_probe(SdCardInfo &info);

/// Read one 512-byte block. `buffer` must have room for 512 bytes.
/// Only valid after a successful probe.
bool sd_card_read_block(uint32_t block_number, uint8_t *buffer);

/// Write one 512-byte block.
///
/// BLOCKS, and the duration is not predictable: the card may perform an
/// internal erase or wear-levelling pass, which the specification allows to
/// take a couple of hundred milliseconds. Never call this from anywhere that
/// the display or the coin scale is depending on.
bool sd_card_write_block(uint32_t block_number, const uint8_t *buffer);

/// Total number of 512-byte blocks, or 0 if no card has been probed.
uint32_t sd_card_block_count(void);

/// True once a probe has succeeded.
bool sd_card_is_ready(void);

/// Human-readable card type, for logging.
const char *sd_card_type_name(SdCardType type);
