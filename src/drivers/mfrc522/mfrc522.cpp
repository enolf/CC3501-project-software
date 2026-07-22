// PiicoDev RFID Module driver (MFRC522 over I2C).
//
// Talking to a card is a two-layer conversation:
//   1. RP2040 <-> MFRC522 over I2C   (write registers, fill/read the FIFO)
//   2. MFRC522 <-> card over 13.56 MHz RF (the Transceive command does this)
// So "send REQA to the card" really means: put 0x26 in the reader's FIFO,
// tell the reader to Transceive, then poll until it says the reply arrived.

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "board.h"
#include "mfrc522.h"
#include "drivers/logging/logging.h"

// --- Low-level register access ---

// One-shot guard: the register helpers below run inside tight polling loops, so
// a disconnected reader could otherwise spew thousands of identical errors. We
// log the first failure of an episode, then stay quiet until a transaction
// succeeds again (which re-arms the report).
static bool bus_error_reported = false;

static void report_bus_error(const char *what)
{
    if (!bus_error_reported) {
        log(LogLevel::ERROR, what);
        bus_error_reported = true;
    }
}

bool mfrc522_write_register(uint8_t reg, uint8_t value)
{
    // I2C write = [register address][value] in one transaction.
    // No address shifting in I2C mode — the register byte goes out as-is.
    uint8_t buf[2] = { reg, value };
    int written = i2c_write_blocking(RFID_I2C_INSTANCE, RFID_I2C_ADDR, buf, 2, false);
    if (written != 2) {
        report_bus_error("mfrc522: I2C register write failed");
        return false;
    }
    bus_error_reported = false;
    return true;
}

uint8_t mfrc522_read_register(uint8_t reg)
{
    // Write the register address (keep the bus with nostop=true),
    // then repeated-start and read one byte back.
    uint8_t value = 0;
    int wrote = i2c_write_blocking(RFID_I2C_INSTANCE, RFID_I2C_ADDR, &reg, 1, true);
    int read  = i2c_read_blocking(RFID_I2C_INSTANCE, RFID_I2C_ADDR, &value, 1, false);
    if (wrote != 1 || read != 1) {
        // A dead bus reads back as 0. Init's identity check rejects 0, and the
        // Transceive/CRC poll loops time out on it, so reads fail safe — callers
        // don't each need to branch, but the failure is still logged (once).
        report_bus_error("mfrc522: I2C register read failed");
        return 0;
    }
    bus_error_reported = false;
    return value;
}

// Read-modify-write helper so we only touch the bits we mean to. Returns false
// if either the read or the write hit a bus error.
static bool mfrc522_set_bits(uint8_t reg, uint8_t mask)
{
    return mfrc522_write_register(reg, mfrc522_read_register(reg) | mask);
}

// --- Core transceive helper ---
// Sends send_len bytes from send_data to the card and collects the reply into
// recv_data (recv_len is in/out: capacity in, bytes received out).
// tx_last_bits: how many bits of the final byte to transmit (0 = all 8).
// REQA is a 7-bit "short frame", which is why this parameter exists at all.
static bool mfrc522_transceive(const uint8_t *send_data, uint8_t send_len,
                               uint8_t *recv_data, uint8_t *recv_len,
                               uint8_t tx_last_bits)
{
    // Stop whatever the chip was doing, clear all IRQ flags, flush the FIFO.
    // These are hot-path writes; a bus failure here surfaces as the poll below
    // timing out (and is logged once by mfrc522_write_register), so we don't
    // branch on each one — that keeps the frame sequence readable.
    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    mfrc522_write_register(MFRC522_REG_COM_IRQ, MFRC522_COMIRQ_CLEAR_ALL);
    mfrc522_write_register(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);

    // Load the outgoing frame into the FIFO.
    for (uint8_t i = 0; i < send_len; i++) {
        mfrc522_write_register(MFRC522_REG_FIFO_DATA, send_data[i]);
    }

    // Start Transceive. Nothing is transmitted yet — Transceive waits for
    // the StartSend bit in BitFramingReg. Setting TxLastBits and StartSend
    // in the same write kicks the frame out onto the RF field.
    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_TRANSCEIVE);
    mfrc522_write_register(MFRC522_REG_BIT_FRAMING,
                           MFRC522_BITFRAMING_START | (tx_last_bits & 0x07));

    // Poll for completion. RxIRq = reply received, IdleIRq = command finished,
    // TimerIRq = our auto-timer expired -> no card. The timer was set up in init
    // to auto-start after transmission (TAuto), so "no answer" resolves itself
    // in ~25 ms instead of hanging forever.
    uint8_t irq;
    uint16_t guard = 2000;  // belt-and-braces bound so a dead chip can't trap us
    do {
        irq = mfrc522_read_register(MFRC522_REG_COM_IRQ);
        if (--guard == 0) {
            return false;
        }
    } while (!(irq & (MFRC522_IRQ_RX | MFRC522_IRQ_IDLE)) && !(irq & MFRC522_IRQ_TIMER));

    if (irq & MFRC522_IRQ_TIMER) {
        // Timer expired: nothing answered. Normal when no card is on the pad.
        return false;
    }

    // Reject the reply if the exchange flagged buffer-overflow, collision,
    // parity or protocol errors — the ones that mean it can't be trusted.
    if (mfrc522_read_register(MFRC522_REG_ERROR) & MFRC522_ERR_INVALID_RX) {
        return false;
    }

    // Pull the reply out of the FIFO, capped at the caller's buffer size.
    uint8_t fifo_count = mfrc522_read_register(MFRC522_REG_FIFO_LEVEL);
    if (fifo_count > *recv_len) {
        fifo_count = *recv_len;
    }
    for (uint8_t i = 0; i < fifo_count; i++) {
        recv_data[i] = mfrc522_read_register(MFRC522_REG_FIFO_DATA);
    }
    *recv_len = fifo_count;

    return true;
}

// Run the chip's CRC coprocessor over len bytes and return the two-byte
// ISO 14443A CRC_A in crc_out (LSB first, ready to append to a frame).
// SELECT frames must carry this CRC; anticollision frames must not.
static bool mfrc522_calc_crc(const uint8_t *data, uint8_t len, uint8_t *crc_out)
{
    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    mfrc522_write_register(MFRC522_REG_DIV_IRQ, MFRC522_DIVIRQ_CRC);  // clear CRCIRq
    mfrc522_write_register(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);

    for (uint8_t i = 0; i < len; i++) {
        mfrc522_write_register(MFRC522_REG_FIFO_DATA, data[i]);
    }
    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_CALC_CRC);

    // CalcCRC runs until we stop it; wait for CRCIRq to signal it has finished.
    uint16_t guard = 5000;
    uint8_t irq;
    do {
        irq = mfrc522_read_register(MFRC522_REG_DIV_IRQ);
        if (--guard == 0) {
            return false;
        }
    } while (!(irq & MFRC522_DIVIRQ_CRC));

    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);  // stop CalcCRC

    // CRC_A goes onto the air low byte first, so return it in that order.
    crc_out[0] = mfrc522_read_register(MFRC522_REG_CRC_RESULT_L);
    crc_out[1] = mfrc522_read_register(MFRC522_REG_CRC_RESULT_H);
    return true;
}

// Run one cascade level. Sends the anticollision request for `sel`
// (SEL_CL1 / SEL_CL2), reads back the 5-byte [uid or CT+uid][BCC] response,
// verifies the BCC, then issues the SELECT (with CRC) so the card advances
// its state ready for the next level. `resp5` receives the raw 5-byte
// anticollision reply. Returns false on any RF/BCC/SELECT failure.
static bool mfrc522_cascade_level(uint8_t sel, uint8_t *resp5)
{
    // Anticollision: [SEL][NVB=ANTICOLL] -> card completes the frame with its
    // UID bytes + BCC. NVB "2 valid bytes from me, no partial bits".
    // Anticollision frames carry no CRC.
    uint8_t anticoll[2] = { sel, PICC_NVB_ANTICOLL };
    uint8_t len = 5;
    if (!mfrc522_transceive(anticoll, 2, resp5, &len, 0) || len != 5) {
        return false;
    }
    // BCC is the XOR checksum of the four preceding bytes.
    if ((resp5[0] ^ resp5[1] ^ resp5[2] ^ resp5[3]) != resp5[4]) {
        return false;
    }

    // SELECT: [SEL][NVB=SELECT][the 5 bytes we just got][CRC_A]. NVB "7 valid
    // bytes, full UID for this level". The card replies with SAK; we don't need
    // to inspect it, only to drive the card into the next state.
    uint8_t select[9] = { sel, PICC_NVB_SELECT,
                          resp5[0], resp5[1], resp5[2], resp5[3], resp5[4] };
    if (!mfrc522_calc_crc(select, 7, &select[7])) {
        return false;
    }
    uint8_t sak[3];
    uint8_t sak_len = sizeof(sak);
    if (!mfrc522_transceive(select, 9, sak, &sak_len, 0)) {
        return false;
    }
    return true;
}

// --- Public API ---

bool mfrc522_init(void)
{
    // Bring up the I2C bus. The instance, pins, baud rate and address all come
    // from board.h — nothing hardware-specific is hard-coded in the driver.
    i2c_init(RFID_I2C_INSTANCE, RFID_I2C_BAUDRATE);
    gpio_set_function(RFID_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(RFID_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(RFID_SDA_PIN);
    gpio_pull_up(RFID_SCL_PIN);

    // Soft reset puts every register back to a known state. The command
    // self-terminates; a short sleep covers the chip's wake-up time.
    mfrc522_write_register(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    sleep_ms(50);

    // Sanity-check that the register bus is really talking before configuring
    // anything. Genuine NXP parts read 0x91/0x92 in VersionReg, but the common
    // clones on PiicoDev-style modules (FM17522 and friends) report other
    // values such as 0x88, 0x12 or 0xB2 while being register-compatible. So we
    // only reject the "dead bus" reads (0x00 = nothing driving the line low,
    // 0xFF = line stuck high); any other stable value means a chip is present.
    uint8_t version = mfrc522_read_register(MFRC522_REG_VERSION);
    if (version == 0x00 || version == 0xFF) {
        return false;
    }

    // Configure the chip. A silent failure on any of these leaves the reader
    // misconfigured (unlike a read, there is no downstream check to catch it),
    // so we attempt every write and bail if any of them failed.
    //
    //  - Timer: TAuto=1 makes the timer auto-start after each transmission, so a
    //    command with no reply times out on its own. The prescaler/reload values
    //    give roughly a 25 ms timeout — long enough for any card, short to poll.
    //  - Force 100% ASK modulation — required for ISO 14443A cards.
    //  - Max receiver gain (48 dB): the reset default (33 dB) can be too weak for
    //    power-hungry / weakly-coupled cards (e.g. a DESFire) on a clone antenna.
    //    Lower MFRC522_RXGAIN_* here to shorten range.
    //  - ModeReg: keeps the reset defaults but sets CRCPreset to 0x6363, the seed
    //    ISO 14443A CRCs are calculated with.
    //  - Antenna drivers come up disabled after reset; without this, nothing is
    //    ever transmitted.
    bool ok = true;
    if (!mfrc522_write_register(MFRC522_REG_T_MODE, MFRC522_TMODE_TAUTO_PSHI))  ok = false;
    if (!mfrc522_write_register(MFRC522_REG_T_PRESCALER, MFRC522_TPRESCALER_LO)) ok = false;
    if (!mfrc522_write_register(MFRC522_REG_T_RELOAD_H, MFRC522_TRELOAD_HI))     ok = false;
    if (!mfrc522_write_register(MFRC522_REG_T_RELOAD_L, MFRC522_TRELOAD_LO))     ok = false;
    if (!mfrc522_write_register(MFRC522_REG_TX_ASK, MFRC522_TXASK_FORCE100))     ok = false;
    if (!mfrc522_write_register(MFRC522_REG_RF_CFG, MFRC522_RXGAIN_48DB))        ok = false;
    if (!mfrc522_write_register(MFRC522_REG_MODE, MFRC522_MODE_CRC6363))         ok = false;
    if (!mfrc522_set_bits(MFRC522_REG_TX_CONTROL, MFRC522_TXCONTROL_ANT_ON))     ok = false;

    return ok;   // helper already logged which transaction failed
}

bool mfrc522_card_present(void)
{
    // REQA is a special 7-bit "short frame": one byte, but only 7 bits of
    // it are sent (tx_last_bits = 7). Any ISO 14443A card in the field
    // answers with a 2-byte ATQA. We only care THAT it answered, not what
    // the ATQA says.
    uint8_t reqa = PICC_CMD_REQA;
    uint8_t atqa[2];
    uint8_t atqa_len = sizeof(atqa);

    return mfrc522_transceive(&reqa, 1, atqa, &atqa_len, 7) && (atqa_len == 2);
}

bool mfrc522_read_card_uid(uint8_t *uid, uint8_t *uid_len)
{
    // --- Cascade level 1 ---
    // The 5-byte reply is either [UID0..UID3][BCC] for a 4-byte card, or
    // [CT=0x88][UID0..UID2][BCC] for a longer UID. The cascade tag 0x88 is
    // the card telling us "there are more UID bytes in the next level".
    uint8_t cl1[5];
    if (!mfrc522_cascade_level(PICC_CMD_SEL_CL1, cl1)) {
        return false;
    }

    if (cl1[0] != PICC_CASCADE_TAG) {
        // Single-size UID: the four bytes are the whole UID.
        uid[0] = cl1[0];
        uid[1] = cl1[1];
        uid[2] = cl1[2];
        uid[3] = cl1[3];
        *uid_len = 4;
        return true;
    }

    // Double-size UID: drop the cascade tag, keep the three real bytes...
    uid[0] = cl1[1];
    uid[1] = cl1[2];
    uid[2] = cl1[3];

    // --- Cascade level 2 ---
    // ...then the second level yields the remaining four bytes (no cascade
    // tag at the final level). The CL1 SELECT inside mfrc522_cascade_level
    // already advanced the card so it will answer SEL_CL2 here.
    uint8_t cl2[5];
    if (!mfrc522_cascade_level(PICC_CMD_SEL_CL2, cl2)) {
        return false;
    }
    uid[3] = cl2[0];
    uid[4] = cl2[1];
    uid[5] = cl2[2];
    uid[6] = cl2[3];
    *uid_len = 7;
    return true;
}
