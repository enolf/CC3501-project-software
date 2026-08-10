// SD card over SPI. See sd_card.h for scope and the blocking warning.
//
// The command set and the initialisation order come from the SD Physical Layer
// Simplified Specification, section 7 (SPI mode). The sequence is fussy and the
// order matters; the comments record why each step is there.

#include "drivers/sd_card/sd_card.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "board.h"
#include "drivers/logging/logging.h"

namespace {

// --- Card commands (SPI mode). CMDn is sent as 0x40 | n. ---
constexpr uint8_t CMD_GO_IDLE_STATE     = 0;   // reset, enter SPI mode
constexpr uint8_t CMD_SEND_IF_COND      = 8;   // voltage range check (v2 only)
constexpr uint8_t CMD_SEND_CSD          = 9;
constexpr uint8_t CMD_SEND_CID          = 10;
constexpr uint8_t CMD_SET_BLOCKLEN      = 16;
constexpr uint8_t CMD_READ_SINGLE_BLOCK = 17;
constexpr uint8_t CMD_WRITE_BLOCK       = 24;
constexpr uint8_t CMD_APP_CMD           = 55;  // prefix for an ACMD
constexpr uint8_t CMD_READ_OCR          = 58;
constexpr uint8_t ACMD_SEND_OP_COND     = 41;  // start initialisation

// --- R1 response bits ---
constexpr uint8_t R1_IDLE_STATE  = 0x01;
constexpr uint8_t R1_READY       = 0x00;
constexpr uint8_t R1_INVALID     = 0x80;  // MSB is always 0 in a valid R1

// Start-of-data token preceding any block the card sends, and the same token
// sent by the host ahead of a single-block write.
constexpr uint8_t DATA_START_TOKEN = 0xFE;

// After a written block the card replies with a data-response byte. The low
// five bits are the status; 0b00101 means accepted.
constexpr uint8_t DATA_RESPONSE_MASK     = 0x1F;
constexpr uint8_t DATA_RESPONSE_ACCEPTED = 0x05;

// How long to allow for the card's internal programming after a write. The
// specification permits a couple of hundred milliseconds when an erase is
// triggered, so this is generous on purpose.
constexpr uint32_t WRITE_TIMEOUT_MS = 500;

constexpr size_t BLOCK_SIZE = 512;

// CRC is disabled in SPI mode, but these two commands are sent before the card
// has been told that, so they need real values. They are constants because the
// arguments are fixed.
constexpr uint8_t CRC_CMD0 = 0x95;   // for CMD0, arg 0x00000000
constexpr uint8_t CRC_CMD8 = 0x87;   // for CMD8, arg 0x000001AA

// CMD8 argument: 2.7-3.6 V supply, check pattern 0xAA. The card echoes both
// back, which is how a v2 card is told apart from a v1 card that rejects CMD8.
constexpr uint32_t CMD8_ARG = 0x000001AA;

// ACMD41 argument with the Host Capacity Support bit set, telling the card the
// host understands block addressing (needed for SDHC).
constexpr uint32_t ACMD41_ARG_HCS = 0x40000000;

// OCR bit 30, Card Capacity Status: set means block-addressed (SDHC/SDXC).
constexpr uint32_t OCR_CCS = 0x40000000;

constexpr uint32_t INIT_TIMEOUT_MS  = 1000;
constexpr uint32_t READ_TIMEOUT_MS  = 300;

SdCardType card_type = SdCardType::None;
uint32_t   card_block_count = 0;

// --- Bus helpers ---
//
// Every SD access sandwiches itself between "take the bus" and "give it back",
// because the display and touch controller share these wires and expect to find
// the bus at their own rate.

void bus_acquire(uint32_t baudrate)
{
    // Both neighbours must be deselected before the card is. Their drivers
    // leave them high, but asserting it here means a bug elsewhere cannot turn
    // into two devices driving MISO at once.
    gpio_put(DISPLAY_CS_PIN, 1);
    gpio_put(TOUCH_CS_PIN, 1);
    spi_set_baudrate(DISPLAY_SPI_INSTANCE, baudrate);
}

void bus_release()
{
    spi_set_baudrate(DISPLAY_SPI_INSTANCE, DISPLAY_SPI_BAUDRATE);
}

uint8_t xfer(uint8_t out)
{
    uint8_t in = 0xFF;
    spi_write_read_blocking(DISPLAY_SPI_INSTANCE, &out, &in, 1);
    return in;
}

void clock_bytes(int count)
{
    for (int i = 0; i < count; i++) {
        (void)xfer(0xFF);
    }
}

void select()
{
    gpio_put(SD_CS_PIN, 0);
}

void deselect()
{
    gpio_put(SD_CS_PIN, 1);
    // Eight extra clocks after deselecting. The card needs them to let go of
    // MISO; without this it can keep driving the line and corrupt the next
    // device's reply — which on this board would be the touch controller.
    clock_bytes(1);
}

/// Wait for the card to stop signalling busy (it holds MISO low while busy).
bool wait_not_busy(uint32_t timeout_ms)
{
    const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (xfer(0xFF) != 0xFF) {
        if (time_reached(deadline)) {
            return false;
        }
    }
    return true;
}

/// Send a command and return its R1 response.
uint8_t send_command(uint8_t command, uint32_t argument, uint8_t crc)
{
    wait_not_busy(READ_TIMEOUT_MS);

    xfer(static_cast<uint8_t>(0x40 | command));
    xfer(static_cast<uint8_t>(argument >> 24));
    xfer(static_cast<uint8_t>(argument >> 16));
    xfer(static_cast<uint8_t>(argument >> 8));
    xfer(static_cast<uint8_t>(argument));
    xfer(crc);

    // The card replies within 8 bytes; the first with its MSB clear is R1.
    for (int i = 0; i < 10; i++) {
        const uint8_t response = xfer(0xFF);
        if ((response & R1_INVALID) == 0) {
            return response;
        }
    }
    return 0xFF;   // nothing valid came back
}

/// Send an application-specific command (CMD55 first, then the ACMD).
uint8_t send_app_command(uint8_t command, uint32_t argument)
{
    send_command(CMD_APP_CMD, 0, 0xFF);
    return send_command(command, argument, 0xFF);
}

/// Read a data block the card is about to send, having already been commanded.
bool read_data_block(uint8_t *buffer, size_t length)
{
    const absolute_time_t deadline = make_timeout_time_ms(READ_TIMEOUT_MS);
    uint8_t token;
    do {
        token = xfer(0xFF);
        if (time_reached(deadline)) {
            return false;
        }
    } while (token == 0xFF);

    if (token != DATA_START_TOKEN) {
        return false;   // an error token rather than data
    }

    for (size_t i = 0; i < length; i++) {
        buffer[i] = xfer(0xFF);
    }

    clock_bytes(2);   // discard the 16-bit CRC; CRC is off in SPI mode
    return true;
}

/// Ask for one of the 16-byte card registers (CMD9 CSD or CMD10 CID).
bool read_register(uint8_t command, uint8_t *out)
{
    if (send_command(command, 0, 0xFF) != R1_READY) {
        return false;
    }
    return read_data_block(out, 16);
}

/// Work out the capacity from the CSD.
uint64_t capacity_from_csd(const uint8_t *csd)
{
    const uint8_t structure_version = static_cast<uint8_t>(csd[0] >> 6);

    if (structure_version == 1) {
        // CSD version 2 (SDHC/SDXC): C_SIZE is 22 bits, and capacity is
        // (C_SIZE + 1) * 512 KB.
        const uint32_t c_size = (static_cast<uint32_t>(csd[7] & 0x3F) << 16) |
                                (static_cast<uint32_t>(csd[8]) << 8) |
                                 static_cast<uint32_t>(csd[9]);
        return (static_cast<uint64_t>(c_size) + 1) * 512ULL * 1024ULL;
    }

    // CSD version 1: capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN
    const uint32_t c_size = (static_cast<uint32_t>(csd[6] & 0x03) << 10) |
                            (static_cast<uint32_t>(csd[7]) << 2) |
                            (static_cast<uint32_t>(csd[8]) >> 6);
    const uint8_t c_size_mult = static_cast<uint8_t>(((csd[9] & 0x03) << 1) |
                                                     (csd[10] >> 7));
    const uint8_t read_bl_len = static_cast<uint8_t>(csd[5] & 0x0F);

    return (static_cast<uint64_t>(c_size) + 1) *
           (1ULL << (c_size_mult + 2)) *
           (1ULL << read_bl_len);
}

} // namespace

void sd_card_init_pins(void)
{
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    gpio_put(SD_CS_PIN, 1);   // deselected, so the card leaves the bus alone
}

bool sd_card_probe(SdCardInfo &info)
{
    info = SdCardInfo{};
    card_type = SdCardType::None;
    card_block_count = 0;

    bus_acquire(SD_SPI_INIT_BAUDRATE);

    // At least 74 clocks with the card DESELECTED, so it can synchronise before
    // being spoken to. Skipping this is the classic reason a card that works
    // elsewhere refuses to initialise here.
    gpio_put(SD_CS_PIN, 1);
    clock_bytes(10);   // 80 clocks

    select();

    // --- CMD0: software reset into SPI mode ---
    if (send_command(CMD_GO_IDLE_STATE, 0, CRC_CMD0) != R1_IDLE_STATE) {
        log(LogLevel::ERROR, "sd: no response to CMD0 - card absent or miswired");
        deselect();
        bus_release();
        return false;
    }

    // --- CMD8: which specification does it follow? ---
    bool version_2 = false;
    const uint8_t if_cond = send_command(CMD_SEND_IF_COND, CMD8_ARG, CRC_CMD8);
    if (if_cond == R1_IDLE_STATE) {
        // R7 carries four more bytes. The last two must echo our voltage range
        // and check pattern, or the card has not understood us.
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) {
            r7[i] = xfer(0xFF);
        }
        if ((r7[2] & 0x0F) != 0x01 || r7[3] != 0xAA) {
            log(LogLevel::ERROR, "sd: CMD8 echo mismatch - voltage unsupported");
            deselect();
            bus_release();
            return false;
        }
        version_2 = true;
    }
    // An illegal-command response to CMD8 simply means a version 1 card.

    // --- ACMD41: start initialisation, and wait for it to finish ---
    const absolute_time_t deadline = make_timeout_time_ms(INIT_TIMEOUT_MS);
    uint8_t op_cond;
    do {
        op_cond = send_app_command(ACMD_SEND_OP_COND,
                                   version_2 ? ACMD41_ARG_HCS : 0);
        if (time_reached(deadline)) {
            log(LogLevel::ERROR, "sd: card never left idle - initialisation timed out");
            deselect();
            bus_release();
            return false;
        }
    } while (op_cond != R1_READY);

    // --- CMD58: standard capacity or high capacity? ---
    // This decides whether block addresses are counted in bytes or in blocks,
    // so getting it wrong reads from entirely the wrong place on the card.
    card_type = version_2 ? SdCardType::V2 : SdCardType::V1;
    if (version_2) {
        if (send_command(CMD_READ_OCR, 0, 0xFF) == R1_READY) {
            uint32_t ocr = 0;
            for (int i = 0; i < 4; i++) {
                ocr = (ocr << 8) | xfer(0xFF);
            }
            if (ocr & OCR_CCS) {
                card_type = SdCardType::V2HC;
            }
        }
    }

    // Byte-addressed cards need their block length set; block-addressed cards
    // are fixed at 512 and ignore this.
    if (card_type != SdCardType::V2HC) {
        send_command(CMD_SET_BLOCKLEN, BLOCK_SIZE, 0xFF);
    }

    // Handshake done, so the bus can go faster for the register reads.
    spi_set_baudrate(DISPLAY_SPI_INSTANCE, SD_SPI_BAUDRATE);

    const bool got_cid = read_register(CMD_SEND_CID, info.cid);
    const bool got_csd = read_register(CMD_SEND_CSD, info.csd);

    deselect();
    bus_release();

    if (!got_cid || !got_csd) {
        log(LogLevel::ERROR, "sd: card initialised but its registers would not read");
        return false;
    }

    info.type = card_type;
    info.capacity_bytes = capacity_from_csd(info.csd);
    card_block_count = static_cast<uint32_t>(info.capacity_bytes / BLOCK_SIZE);

    // CID layout: [0] manufacturer, [1..2] OEM, [3..7] product name,
    // [8] revision, [9..12] serial, [13..14] manufacture date.
    memcpy(info.product_name, &info.cid[3], 5);
    info.product_name[5] = '\0';

    const uint16_t date = static_cast<uint16_t>((info.cid[13] << 8) | info.cid[14]);
    info.manufacture_year = static_cast<uint16_t>(2000 + ((date >> 4) & 0xFF));
    info.manufacture_month = static_cast<uint8_t>(date & 0x0F);

    return true;
}

bool sd_card_read_block(uint32_t block_number, uint8_t *buffer)
{
    if (card_type == SdCardType::None) {
        return false;
    }

    // High capacity cards address in blocks; everything older addresses in
    // bytes. Same physical block, different number on the wire.
    const uint32_t address = (card_type == SdCardType::V2HC)
                                 ? block_number
                                 : block_number * BLOCK_SIZE;

    bus_acquire(SD_SPI_BAUDRATE);
    select();

    bool ok = false;
    if (send_command(CMD_READ_SINGLE_BLOCK, address, 0xFF) == R1_READY) {
        ok = read_data_block(buffer, BLOCK_SIZE);
    }

    deselect();
    bus_release();
    return ok;
}

bool sd_card_write_block(uint32_t block_number, const uint8_t *buffer)
{
    if (card_type == SdCardType::None) {
        return false;
    }

    const uint32_t address = (card_type == SdCardType::V2HC)
                                 ? block_number
                                 : block_number * BLOCK_SIZE;

    bus_acquire(SD_SPI_BAUDRATE);
    select();

    bool ok = false;
    if (send_command(CMD_WRITE_BLOCK, address, 0xFF) == R1_READY) {
        // One byte of gap, then the start token, then the payload.
        xfer(0xFF);
        xfer(DATA_START_TOKEN);
        for (size_t i = 0; i < BLOCK_SIZE; i++) {
            xfer(buffer[i]);
        }
        clock_bytes(2);   // dummy CRC; CRC checking is off in SPI mode

        const uint8_t response = xfer(0xFF);
        if ((response & DATA_RESPONSE_MASK) == DATA_RESPONSE_ACCEPTED) {
            // The card now holds MISO low while it programs the flash. It must
            // be allowed to finish before the bus is handed back, or the next
            // device to use it finds the card still driving the line.
            ok = wait_not_busy(WRITE_TIMEOUT_MS);
            if (!ok) {
                log(LogLevel::ERROR, "sd: card still busy after write timeout");
            }
        } else {
            log(LogLevel::ERROR, "sd: block write rejected by card");
        }
    }

    deselect();
    bus_release();
    return ok;
}

uint32_t sd_card_block_count(void)
{
    return card_block_count;
}

bool sd_card_is_ready(void)
{
    return card_type != SdCardType::None;
}

const char *sd_card_type_name(SdCardType type)
{
    switch (type) {
        case SdCardType::None: return "none";
        case SdCardType::V1:   return "SD v1";
        case SdCardType::V2:   return "SD v2";
        case SdCardType::V2HC: return "SDHC/SDXC";
    }
    return "unknown";
}
